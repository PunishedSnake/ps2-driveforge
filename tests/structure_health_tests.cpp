#include "ps2hdd/fhdb_rescue.hpp"
#include "ps2hdd/fhdb_rescue_capture.hpp"
#include "ps2hdd/fhdb_restore.hpp"
#include "ps2hdd/structure_health.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
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
    std::string display_name() const override { return "structure-health.img"; }

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
                      (std::string("ps2-driveforge-structure-health-") + name);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path);
    return path;
}

constexpr std::size_t kDiskBytes = 0x4000U * 512U;
constexpr std::uint32_t kPayloadStart = 0x2000;

MemoryDisk disk_with(const std::array<std::byte, 1024>& master, bool kelf = true)
{
    MemoryDisk disk(kDiskBytes);
    disk.set_master(master);
    if (kelf) {
        const auto payload = make_kelf_payload();
        check(disk.write(static_cast<std::uint64_t>(kPayloadStart) * 512U, payload), "seed payload");
    }
    disk.write_offsets.clear();
    return disk;
}

void make_jail(std::array<std::byte, 1024>& master)
{
    std::memcpy(master.data() + 0xe0, "APAJ-A2", 7);
    const std::size_t entry = 0x1be;
    master[entry + 4] = std::byte{0x17};
    store_u32(master.data() + entry + 8, 0);
    store_u32(master.data() + entry + 12, 0x4000);
    master[0x1fe] = std::byte{0x55};
    master[0x1ff] = std::byte{0xaa};
    store_u32(master.data(), checksum(master));
}

using namespace ps2hdd::health;

void healthy_and_disabled()
{
    auto disk = disk_with(make_master(kPayloadStart, 1));
    const auto report = assess(disk);
    check(report.verdict == Verdict::healthy, "valid KELF pointer is healthy");
    check(report.pointer == PointerState::valid, "pointer classified valid");
    check(report.actions.size() == 1 && report.actions[0] == Action::capture_rescue,
          "healthy disk only suggests a Rescue Capsule");
    check(disk.write_offsets.empty(), "assessment never writes");

    auto off = disk_with(make_master(0, 0), false);
    const auto disabled = assess(off);
    check(disabled.verdict == Verdict::healthy && disabled.pointer == PointerState::disabled &&
          disabled.actions.empty(), "disabled pointer is healthy with nothing to do");
    check(!render(disabled).empty(), "report renders");
}

void bad_pointers_recommend_disable()
{
    struct Case { std::uint32_t start, size; bool kelf; PointerState expect; const char* name; };
    const Case cases[] = {
        {kPayloadStart, 0, true, PointerState::half_empty, "half-empty"},
        {0x100, 1, true, PointerState::out_of_bounds, "before program area"},
        {0x3fff, 8, true, PointerState::out_of_bounds, "past __mbr"},
        {kPayloadStart, 1, false, PointerState::not_kelf, "zeroed payload"},
    };
    for (const auto& c : cases) {
        auto disk = disk_with(make_master(c.start, c.size), c.kelf);
        const auto report = assess(disk);
        if (report.pointer != c.expect) std::cerr << c.name << ": " << pointer_name(report.pointer) << '\n';
        check(report.pointer == c.expect, "pointer state classified");
        check(report.verdict == Verdict::repairable && recommends(report, Action::disable_bootstrap) &&
              !recommends(report, Action::capture_rescue), "bad pointer recommends disable, not backup");
    }
}

void disable_bootstrap_flow()
{
    const auto dir = temp_dir("disable");
    // Non-KELF payload: rescue capture still succeeds (without VALID_KELF), so
    // both artifacts exist and the pointer can be restored later.
    auto disk = disk_with(make_master(kPayloadStart, 1), false);
    const auto before_payload = disk.bytes_at(kPayloadStart * 512U, 512);
    const auto result = ps2hdd::fhdb::disable_bootstrap(disk, dir);
    if (!result.ok) std::cerr << result.error << '\n';
    check(result.ok && result.pointer_cleared && result.final_apa_verified, "disable succeeds");
    check(result.previous_start == kPayloadStart && result.previous_sectors == 1, "previous pointer recorded");
    check(std::filesystem::exists(result.hddmbr_path), "HDDMBR saved before write");
    check(!result.rescue_path.empty() && std::filesystem::exists(result.rescue_path), "rescue capsule saved");
    const auto master = disk.master();
    check(load_u32(master.data() + 0x130) == 0 && load_u32(master.data() + 0x134) == 0, "pointer cleared");
    check(load_u32(master.data()) == checksum(master), "checksum recomputed");
    check(ps2hdd::fhdb::same_disk_identity(master, result.source_master), "only pointer + checksum changed");
    check(disk.bytes_at(kPayloadStart * 512U, 512) == before_payload, "payload sectors untouched");
    check(disk.write_offsets.size() == 1 && disk.write_offsets[0] == 0, "single master write");
    const auto after = assess(disk);
    check(after.verdict == Verdict::healthy && after.pointer == PointerState::disabled, "healthy after disable");

    // Half-empty: capture refuses, disable must still proceed on HDDMBR alone.
    const auto dir2 = temp_dir("disable-half");
    auto torn = disk_with(make_master(kPayloadStart, 0));
    const auto torn_result = ps2hdd::fhdb::disable_bootstrap(torn, dir2);
    check(torn_result.ok && torn_result.rescue_path.empty() && !torn_result.warning.empty(),
          "failed rescue capture never blocks disable");

    // Already disabled: nothing written.
    auto off = disk_with(make_master(0, 0), false);
    const auto noop = ps2hdd::fhdb::disable_bootstrap(off, temp_dir("disable-noop"));
    check(noop.ok && noop.already_disabled && off.write_offsets.empty(), "already disabled is a no-op");

    check(!ps2hdd::fhdb::disable_bootstrap(disk, {}).ok, "safety directory is mandatory");
}

void master_damage()
{
    auto master = make_master(kPayloadStart, 1);
    master[0x004] = std::byte{'B'};  // single identity bit-flip, checksum left stale
    auto disk = disk_with(master);
    const auto report = assess(disk);
    check(!report.master_canonical, "damaged master detected");
    check(report.verdict == Verdict::repairable && !report.actions.empty() &&
          report.actions[0] == Action::repair_master, "single field damage recommends master repair first");
    check(report.pointer == PointerState::unknown, "pointer not evaluated through a damaged master");

    auto unexplained = make_master(kPayloadStart, 1);
    store_u32(unexplained.data(), 0x12345678U);
    auto bad = disk_with(unexplained);
    const auto ambiguous = assess(bad);
    check(ambiguous.verdict == Verdict::damaged && recommends(ambiguous, Action::forensic_scan) &&
          !recommends(ambiguous, Action::repair_master), "uncorroborated checksum goes to forensic analysis");
}

void ownership()
{
    auto master = make_master(kPayloadStart, 1);
    master[0x1be + 4] = std::byte{0xee};
    master[0x1fe] = std::byte{0x55};
    master[0x1ff] = std::byte{0xaa};
    store_u32(master.data(), checksum(master));
    auto gpt = disk_with(master);
    const auto report = assess(gpt);
    check(report.verdict == Verdict::blocked && report.pc_owned && report.actions.empty(),
          "protective GPT blocks every action");
    check(!ps2hdd::fhdb::disable_bootstrap(gpt, temp_dir("gpt")).ok && gpt.write_offsets.empty(),
          "disable refuses a PC-owned master");

    auto jail_master = make_master(kPayloadStart, 1);
    make_jail(jail_master);
    check(ps2hdd::fhdb::is_apa_jail_master(jail_master) && !ps2hdd::fhdb::is_pc_owned_master(jail_master),
          "signed APA-Jail master is APA-owned");
    auto jail = disk_with(jail_master);
    const auto jail_report = assess(jail);
    if (jail_report.verdict != Verdict::healthy) std::cerr << render(jail_report);
    check(jail_report.apa_jail && !jail_report.pc_owned && jail_report.pointer == PointerState::valid,
          "APA-Jail pointer is evaluated");
    const auto capsule = ps2hdd::fhdb::capture_rescue_image(jail);
    check(capsule.ok, "Rescue Capsule capture works on APA-Jail");
    const auto disabled = ps2hdd::fhdb::disable_bootstrap(jail, temp_dir("jail"));
    check(disabled.ok, "disable works on APA-Jail");
    const auto after = jail.master();
    check(std::equal(after.begin() + 0x1be, after.begin() + 0x200, jail_master.begin() + 0x1be) &&
          std::memcmp(after.data() + 0xe0, "APAJ-A2", 7) == 0, "jail PC table and signature preserved");

    auto foreign = make_master(kPayloadStart, 1);
    foreign[0x1be + 4] = std::byte{0x07};
    foreign[0x1fe] = std::byte{0x55};
    foreign[0x1ff] = std::byte{0xaa};
    store_u32(foreign.data(), checksum(foreign));
    check(ps2hdd::fhdb::is_pc_owned_master(foreign), "unsigned PC MBR stays PC-owned");
}

} // namespace

int main()
{
    try {
        healthy_and_disabled();
        bad_pointers_recommend_disable();
        disable_bootstrap_flow();
        master_damage();
        ownership();
    } catch (const std::exception& error) {
        std::cerr << "structure health test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Structure health tests passed\n";
    return 0;
}
