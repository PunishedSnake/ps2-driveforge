#include "ps2hdd/opl_partition.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ZeroDevice final : public ps2hdd::BlockDevice {
public:
    explicit ZeroDevice(std::uint64_t bytes) : bytes_(bytes) {}
    std::uint64_t size_bytes() const override { return bytes_; }
    std::string display_name() const override { return "resolver-fixture"; }
    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        ++reads;
        if (offset > bytes_ || out.size() > bytes_ - offset) {
            return false;
        }
        std::fill(out.begin(), out.end(), std::byte{0});
        return true;
    }
    std::size_t reads{};

private:
    std::uint64_t bytes_{};
};

ps2hdd::apa::ScanResult clean_scan()
{
    ps2hdd::apa::ScanResult scan;
    scan.mbr_valid = true;
    scan.apa_version = 2;
    return scan;
}

ps2hdd::apa::Partition partition(std::string id, std::uint16_t type,
                                 std::uint32_t start = 0x40000,
                                 std::uint32_t length = 0x40000)
{
    ps2hdd::apa::Partition p;
    p.id = std::move(id);
    p.type = type;
    p.start_lba = start;
    p.length_sectors = length;
    p.total_sectors = length;
    return p;
}

void test_configured_non_pfs_refused_without_io()
{
    auto scan = clean_scan();
    scan.partitions.push_back(partition("+OPL", ps2hdd::apa::kTypeHdl));
    ZeroDevice device(512ULL * 1024ULL * 1024ULL);
    const auto result = ps2hdd::opl::resolve_data_partition(device, scan, "+OPL");
    check(!result.ok, "configured non-PFS partition was accepted");
    check(device.reads == 0, "resolver probed payload after type mismatch");
}

void test_missing_configured_partition_refused_without_io()
{
    auto scan = clean_scan();
    scan.partitions.push_back(partition("+DATA", ps2hdd::apa::kTypePfs));
    ZeroDevice device(512ULL * 1024ULL * 1024ULL);
    const auto result = ps2hdd::opl::resolve_data_partition(device, scan, "+OPL");
    check(!result.ok, "missing configured partition was accepted");
    check(device.reads == 0, "resolver probed unrelated PFS for explicit missing target");
}

void test_invalid_pfs_is_not_auto_selected()
{
    auto scan = clean_scan();
    scan.partitions.push_back(partition("+OPL", ps2hdd::apa::kTypePfs));
    ZeroDevice device(512ULL * 1024ULL * 1024ULL);
    const auto result = ps2hdd::opl::resolve_data_partition(device, scan);
    check(!result.ok, "invalid PFS was automatically selected");
    check(!result.candidates.empty(), "invalid PFS evidence was not reported");
    check(!result.candidates.front().pfs_valid, "zero-filled PFS was marked valid");
    check(device.reads != 0, "automatic resolver never probed PFS");
}

void test_dirty_apa_refused_without_io()
{
    auto scan = clean_scan();
    scan.issues.push_back({ps2hdd::apa::IssueSeverity::error, 0, "fixture corruption"});
    scan.partitions.push_back(partition("+OPL", ps2hdd::apa::kTypePfs));
    ZeroDevice device(512ULL * 1024ULL * 1024ULL);
    const auto result = ps2hdd::opl::resolve_data_partition(device, scan);
    check(!result.ok, "dirty APA scan was accepted");
    check(device.reads == 0, "dirty APA scan caused PFS reads");
}

} // namespace

int main()
{
    try {
        test_configured_non_pfs_refused_without_io();
        test_missing_configured_partition_refused_without_io();
        test_invalid_pfs_is_not_auto_selected();
        test_dirty_apa_refused_without_io();
        std::cout << "OPL partition resolver tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OPL partition resolver test failure: " << error.what() << '\n';
        return 1;
    }
}
