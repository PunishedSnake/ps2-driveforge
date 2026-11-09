#include "ps2hdd/hdl_install_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void store_u32_both(std::byte* p, std::uint32_t value)
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
    p[4] = p[3];
    p[5] = p[2];
    p[6] = p[1];
    p[7] = p[0];
}

class MemoryDevice final : public ps2hdd::BlockDevice {
public:
    explicit MemoryDevice(std::size_t bytes) : bytes_(bytes) {}

    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "synthetic-game-iso"; }
    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - offset) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }
    std::span<std::byte> bytes() { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

void write_record(std::span<std::byte> target, std::uint32_t extent_lba,
                  std::uint32_t data_bytes, std::uint8_t flags,
                  std::string_view identifier)
{
    const std::size_t padding = identifier.size() % 2 == 0 ? 1 : 0;
    const std::size_t length = 33 + identifier.size() + padding;
    target[0] = static_cast<std::byte>(length);
    store_u32_both(target.data() + 2, extent_lba);
    store_u32_both(target.data() + 10, data_bytes);
    target[25] = static_cast<std::byte>(flags);
    target[28] = std::byte{1};
    target[31] = std::byte{1};
    target[32] = static_cast<std::byte>(identifier.size());
    std::memcpy(target.data() + 33, identifier.data(), identifier.size());
}

MemoryDevice make_iso()
{
    constexpr std::uint32_t root_lba = 20;
    constexpr std::uint32_t cnf_lba = 21;
    constexpr std::size_t sectors = 24;
    constexpr std::string_view cnf = "BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.00\r\n";
    MemoryDevice image(sectors * ps2hdd::iso::kSectorSize);

    auto pvd = image.bytes().subspan(16 * ps2hdd::iso::kSectorSize, ps2hdd::iso::kSectorSize);
    pvd[0] = std::byte{1};
    std::memcpy(pvd.data() + 1, "CD001", 5);
    pvd[6] = std::byte{1};
    const std::string root_id(1, '\0');
    write_record(pvd.subspan(156), root_lba, ps2hdd::iso::kSectorSize, 0x02, root_id);

    auto root = image.bytes().subspan(root_lba * ps2hdd::iso::kSectorSize, ps2hdd::iso::kSectorSize);
    const std::string dot(1, '\0');
    write_record(root, root_lba, ps2hdd::iso::kSectorSize, 0x02, dot);
    const auto used = std::to_integer<unsigned char>(root[0]);
    write_record(root.subspan(used), cnf_lba, static_cast<std::uint32_t>(cnf.size()), 0, "SYSTEM.CNF;1");
    std::memcpy(image.bytes().data() + cnf_lba * ps2hdd::iso::kSectorSize, cnf.data(), cnf.size());
    return image;
}

ps2hdd::apa::ScanResult make_disk_scan()
{
    ps2hdd::apa::ScanResult scan;
    scan.mbr_valid = true;
    scan.apa_version = 2;

    ps2hdd::apa::Partition mbr;
    mbr.id = "__mbr";
    mbr.start_lba = 0;
    mbr.length_sectors = ps2hdd::apa::kAllocationChunkSectors;
    mbr.total_sectors = mbr.length_sectors;
    mbr.type = ps2hdd::apa::kTypeMbr;
    mbr.prev_lba = ps2hdd::apa::kAllocationChunkSectors;
    mbr.next_lba = ps2hdd::apa::kAllocationChunkSectors;

    ps2hdd::apa::Partition existing;
    existing.id = "+OPL";
    existing.start_lba = ps2hdd::apa::kAllocationChunkSectors;
    existing.length_sectors = ps2hdd::apa::kAllocationChunkSectors;
    existing.total_sectors = existing.length_sectors;
    existing.type = ps2hdd::apa::kTypePfs;
    existing.prev_lba = 0;
    existing.next_lba = 0;

    scan.partitions.push_back(std::move(mbr));
    scan.partitions.push_back(std::move(existing));
    return scan;
}

void test_partition_id_generation()
{
    check(ps2hdd::hdl::make_partition_id("SLUS_123.45", "Metal Gear Solid 2") ==
              "PP.SLUS-12345..METAL_GEAR_SOLID",
          "visible partition ID normalization mismatch");
    check(ps2hdd::hdl::make_partition_id("sles_543.21", "game! name", true) ==
              "__.SLES-54321..GAME__NAME",
          "hidden partition ID normalization mismatch");
    check(ps2hdd::hdl::make_partition_id("GAME.ELF", "Title").empty(),
          "invalid startup should not produce an HDL partition ID");
}

void test_install_preflight_combines_iso_and_apa()
{
    auto iso = make_iso();
    const auto scan = make_disk_scan();
    const auto plan = ps2hdd::hdl::plan_install(scan,
        static_cast<std::uint64_t>(64) * ps2hdd::apa::kAllocationChunkSectors * ps2hdd::apa::kSectorSize,
        iso, "Test Game");

    check(plan.ok, "valid ISO + clean disk should produce an install plan");
    check(plan.source.startup == "SLUS_123.45", "install plan startup mismatch");
    check(plan.partition_id == "PP.SLUS-12345..TEST_GAME", "install plan partition ID mismatch");
    check(plan.allocation.ok && !plan.allocation.extents.empty(), "install plan has no APA allocation");
    check(plan.allocation.usable_payload_bytes >= iso.size_bytes(), "planned allocation cannot fit source ISO");
}

void test_bad_disk_is_refused_before_allocation()
{
    auto iso = make_iso();
    auto scan = make_disk_scan();
    scan.issues.push_back({ps2hdd::apa::IssueSeverity::error, 0, "synthetic fatal error"});
    const auto plan = ps2hdd::hdl::plan_install(scan,
        static_cast<std::uint64_t>(64) * ps2hdd::apa::kAllocationChunkSectors * ps2hdd::apa::kSectorSize,
        iso, "Test Game");
    check(!plan.ok, "fatal APA diagnostics must block install preflight");
}

} // namespace

int main()
{
    try {
        test_partition_id_generation();
        test_install_preflight_combines_iso_and_apa();
        test_bad_disk_is_refused_before_allocation();
        std::cout << "HDL install plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HDL install plan tests failed: " << error.what() << '\n';
        return 1;
    }
}
