#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/fhdb_restore.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t load_u32(const std::byte* p)
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

void store_u16(std::byte* p, std::uint16_t value)
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void store_u32(std::byte* p, std::uint32_t value)
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

std::uint32_t checksum(std::span<const std::byte, 1024> header)
{
    std::uint32_t sum = 0;
    for (std::size_t word = 1; word < 256; ++word) {
        sum += load_u32(header.data() + word * 4);
    }
    return sum;
}

std::array<std::byte, 1024> make_master(std::uint32_t osd_start = 0,
                                        std::uint32_t osd_size = 0,
                                        std::byte identity_byte = std::byte{0})
{
    std::array<std::byte, 1024> header{};
    std::memcpy(header.data() + 0x004, "APA\0", 4);
    std::memcpy(header.data() + 0x010, "__mbr", 5);
    store_u32(header.data() + 0x040, 0);
    store_u32(header.data() + 0x044, 0x4000);
    store_u16(header.data() + 0x048, 1);
    std::memcpy(header.data() + 0x100, "Sony Computer Entertainment Inc.", 32);
    store_u32(header.data() + 0x120, 2);
    store_u32(header.data() + 0x130, osd_start);
    store_u32(header.data() + 0x134, osd_size);
    header[0x200] = identity_byte;
    store_u32(header.data(), checksum(header));
    return header;
}

std::vector<std::byte> make_kelf_payload()
{
    std::vector<std::byte> payload(512, std::byte{0});
    constexpr std::uint16_t header_size = 72;
    constexpr std::uint32_t elf_size = 8;
    store_u32(payload.data() + 0x10, elf_size);
    store_u16(payload.data() + 0x14, header_size);
    store_u16(payload.data() + 0x18, 0);
    store_u16(payload.data() + 0x1a, 0);
    for (std::size_t i = header_size; i < header_size + elf_size; ++i) {
        payload[i] = static_cast<std::byte>(0x30U + static_cast<unsigned>(i - header_size));
    }
    return payload;
}

class MemoryDisk final : public ps2hdd::WritableBlockDevice {
public:
    explicit MemoryDisk(std::size_t bytes) : bytes_(bytes) {}

    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "fhdb-restore.img"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - offset) return false;
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        if (offset > bytes_.size() || in.size() > bytes_.size() - offset) return false;
        write_offsets.push_back(offset);
        std::copy(in.begin(), in.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }

    bool flush() override
    {
        ++flushes;
        return true;
    }

    void set_master(const std::array<std::byte, 1024>& master)
    {
        std::copy(master.begin(), master.end(), bytes_.begin());
    }

    std::array<std::byte, 1024> master() const
    {
        std::array<std::byte, 1024> out{};
        std::copy_n(bytes_.begin(), out.size(), out.begin());
        return out;
    }

    std::vector<std::byte> bytes_at(std::size_t offset, std::size_t count) const
    {
        return {bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes_.begin() + static_cast<std::ptrdiff_t>(offset + count)};
    }

    std::vector<std::uint64_t> write_offsets;
    std::size_t flushes{};

private:
    std::vector<std::byte> bytes_;
};

std::filesystem::path temp_dir(const char* name)
{
    const auto path = std::filesystem::temp_directory_path() /
                      (std::string("ps2-driveforge-fhdb-restore-") + name);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path);
    return path;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "open fixture file");
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    check(static_cast<bool>(output), "write fixture file");
}

ps2hdd::fhdb::RescueImageResult make_full_rescue(
    const std::array<std::byte, 1024>& saved_master)
{
    const auto payload = make_kelf_payload();
    auto rescue = ps2hdd::fhdb::build_rescue_image(
        saved_master, payload, 0x2000, 1,
        "0220JC20060905", "FHDB / custom", "high");
    check(rescue.ok, "build full rescue fixture");
    check((rescue.info.flags & ps2hdd::fhdb::kRescueFlagValidKelf) != 0,
          "fixture rescue must contain a validated KELF");
    return rescue;
}

void test_full_rescue_payload_precedes_pointer_and_saves_current_master()
{
    MemoryDisk disk(8U * 1024U * 1024U);
    const auto current = make_master();
    const auto saved = make_master(0x2000, 1);
    disk.set_master(current);

    const auto artifacts = temp_dir("full-artifacts");
    const auto safety = temp_dir("full-safety");
    const auto rescue = make_full_rescue(saved);
    check(ps2hdd::fhdb::save_hddrescue(artifacts, rescue).ok, "save rescue fixture");

    const auto plan = ps2hdd::fhdb::plan_bootstrap_restore(disk, artifacts);
    check(plan.ok && plan.kind == ps2hdd::fhdb::BootstrapRestoreKind::rescue_payload,
          "full rescue should win discovery");
    const auto result = ps2hdd::fhdb::apply_bootstrap_restore(disk, plan, safety);
    check(result.ok && result.payload_verified && result.pointer_published && result.final_apa_verified,
          "full rescue restore should verify payload, pointer and APA");
    check(result.safety_backup_path.filename() == "HDDMBR.BIN",
          "restore must save canonical current-master safety backup first");
    check(disk.write_offsets.size() >= 2 && disk.write_offsets.front() == 0x2000ULL * 512ULL &&
              disk.write_offsets[1] == 0,
          "full restore must publish payload before APA pointer");
    check(disk.bytes_at(0x2000U * 512U, rescue.payload.size()) == rescue.payload,
          "restored payload bytes mismatch");
    const auto master = disk.master();
    check(load_u32(master.data() + 0x130) == 0x2000 &&
              load_u32(master.data() + 0x134) == 1,
          "restored pointer mismatch");
    check(master[0x200] == current[0x200],
          "restore must preserve unrelated live master metadata");

    std::filesystem::remove_all(artifacts);
    std::filesystem::remove_all(safety);
}

void test_header_only_rescue_allows_legacy_fhdbmbr_fallback()
{
    MemoryDisk disk(8U * 1024U * 1024U);
    const auto current = make_master();
    const auto enabled = make_master(0x2000, 1);
    disk.set_master(current);
    const auto artifacts = temp_dir("legacy-artifacts");

    const auto header_only = ps2hdd::fhdb::build_rescue_image(current, {}, 0, 0,
                                                               {}, "disabled", "high");
    check(header_only.ok && ps2hdd::fhdb::save_hddrescue(artifacts, header_only).ok,
          "save header-only rescue fixture");
    write_file(artifacts / "FHDBMBR.BIN", enabled);

    const auto plan = ps2hdd::fhdb::plan_bootstrap_restore(disk, artifacts);
    check(plan.ok && plan.kind == ps2hdd::fhdb::BootstrapRestoreKind::legacy_pointer &&
              plan.source_path.filename() == "FHDBMBR.BIN",
          "header-only rescue should permit legacy FHDBMBR fallback");
    std::filesystem::remove_all(artifacts);
}

void test_invalid_rescue_blocks_legacy_fallback_and_rejects_mutation_journal_magic()
{
    MemoryDisk disk(8U * 1024U * 1024U);
    const auto current = make_master();
    const auto enabled = make_master(0x2000, 1);
    disk.set_master(current);
    const auto artifacts = temp_dir("invalid-artifacts");

    const std::array<std::byte, 8> journal_magic{
        std::byte{'P'}, std::byte{'S'}, std::byte{'2'}, std::byte{'D'},
        std::byte{'F'}, std::byte{'R'}, std::byte{'C'}, std::byte{'1'}};
    write_file(artifacts / "HDDRESCUE.BIN", journal_magic);
    write_file(artifacts / "HDDMBR.BIN", enabled);

    const auto plan = ps2hdd::fhdb::plan_bootstrap_restore(disk, artifacts);
    check(!plan.ok && plan.kind == ps2hdd::fhdb::BootstrapRestoreKind::none,
          "invalid HDDRESCUE must block legacy fallback");
    check(plan.error.find("rescue") != std::string::npos ||
              plan.error.find("Recovery") != std::string::npos ||
              plan.error.find("FHDB") != std::string::npos,
          "mutation journal disguised as HDDRESCUE should fail as a rescue format");
    std::filesystem::remove_all(artifacts);
}

void test_wrong_disk_rescue_blocks_legacy_fallback()
{
    MemoryDisk disk(8U * 1024U * 1024U);
    const auto current = make_master(0, 0, std::byte{0x11});
    const auto foreign = make_master(0x2000, 1, std::byte{0x22});
    const auto local_legacy = make_master(0x2000, 1, std::byte{0x11});
    disk.set_master(current);
    const auto artifacts = temp_dir("wrong-disk");

    check(ps2hdd::fhdb::save_hddrescue(artifacts, make_full_rescue(foreign)).ok,
          "save foreign rescue fixture");
    write_file(artifacts / "HDDMBR.BIN", local_legacy);
    const auto plan = ps2hdd::fhdb::plan_bootstrap_restore(disk, artifacts);
    check(!plan.ok && plan.error.find("different PS2 HDD") != std::string::npos,
          "wrong-disk rescue must block local legacy fallback");
    std::filesystem::remove_all(artifacts);
}

void test_stale_plan_refuses_before_safety_backup_or_write()
{
    MemoryDisk disk(8U * 1024U * 1024U);
    const auto current = make_master();
    const auto saved = make_master(0x2000, 1);
    disk.set_master(current);
    const auto artifacts = temp_dir("stale-artifacts");
    const auto safety = temp_dir("stale-safety");
    check(ps2hdd::fhdb::save_hddrescue(artifacts, make_full_rescue(saved)).ok,
          "save stale-plan rescue fixture");
    const auto plan = ps2hdd::fhdb::plan_bootstrap_restore(disk, artifacts);
    check(plan.ok, "stale plan preflight failed");

    auto changed = current;
    store_u32(changed.data() + 0x130, 0x2000);
    store_u32(changed.data() + 0x134, 1);
    store_u32(changed.data(), checksum(changed));
    disk.set_master(changed);
    const auto result = ps2hdd::fhdb::apply_bootstrap_restore(disk, plan, safety);
    check(!result.ok && result.error.find("changed after restore preflight") != std::string::npos,
          "stale restore plan must be refused");
    check(disk.write_offsets.empty(), "stale plan refusal must happen before device writes");
    check(std::filesystem::is_empty(safety), "stale plan refusal must happen before safety artifact creation");

    std::filesystem::remove_all(artifacts);
    std::filesystem::remove_all(safety);
}

} // namespace

int main()
{
    try {
        test_full_rescue_payload_precedes_pointer_and_saves_current_master();
        test_header_only_rescue_allows_legacy_fhdbmbr_fallback();
        test_invalid_rescue_blocks_legacy_fallback_and_rejects_mutation_journal_magic();
        test_wrong_disk_rescue_blocks_legacy_fallback();
        test_stale_plan_refuses_before_safety_backup_or_write();
        std::cout << "FHDB restore interoperability tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FHDB restore interoperability tests failed: " << error.what() << '\n';
        return 1;
    }
}
