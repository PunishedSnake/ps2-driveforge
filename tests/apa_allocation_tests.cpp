#include "ps2hdd/apa_allocation.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ps2hdd::apa::ScanResult make_scan(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& runs)
{
    ps2hdd::apa::ScanResult scan;
    scan.mbr_valid = true;
    scan.apa_version = 2;
    scan.partitions.reserve(runs.size());

    for (std::size_t i = 0; i < runs.size(); ++i) {
        ps2hdd::apa::Partition partition;
        partition.id = i == 0 ? "__mbr" : ("existing-" + std::to_string(i));
        partition.start_lba = runs[i].first * ps2hdd::apa::kAllocationChunkSectors;
        partition.length_sectors = runs[i].second * ps2hdd::apa::kAllocationChunkSectors;
        partition.total_sectors = partition.length_sectors;
        partition.type = i == 0 ? ps2hdd::apa::kTypeMbr : ps2hdd::apa::kTypePfs;
        partition.prev_lba =
            (i == 0 ? runs.back().first : runs[i - 1].first) * ps2hdd::apa::kAllocationChunkSectors;
        partition.next_lba = i + 1 < runs.size()
                                 ? runs[i + 1].first * ps2hdd::apa::kAllocationChunkSectors
                                 : 0;
        scan.partitions.push_back(std::move(partition));
    }
    return scan;
}

std::uint64_t disk_bytes(std::uint32_t chunks)
{
    return static_cast<std::uint64_t>(chunks) *
           ps2hdd::apa::kAllocationChunkSectors * ps2hdd::apa::kSectorSize;
}

std::uint64_t mib(std::uint64_t value)
{
    return value * 1024ULL * 1024ULL;
}

void test_contiguous_chunks_fold_into_aligned_extent()
{
    // Chunks 0 and 1 are occupied. On a 64-chunk disk the maximum folded
    // extent is two chunks, so free chunks 2+3 become one 256 MiB main extent.
    const auto scan = make_scan({{0, 1}, {1, 1}});
    const auto plan = ps2hdd::apa::plan_hdl_allocation(scan, disk_bytes(64), mib(200));

    check(plan.ok, "aligned contiguous allocation should succeed");
    check(plan.extents.size() == 1, "two aligned chunks should fold into one extent");
    check(plan.extents[0].start_lba == 2 * ps2hdd::apa::kAllocationChunkSectors,
          "unexpected main extent start");
    check(plan.extents[0].length_sectors == 2 * ps2hdd::apa::kAllocationChunkSectors,
          "unexpected folded main extent length");
    check(plan.overhead_bytes == mib(4), "one-part HDL overhead should be 4 MiB");
    check(plan.usable_payload_bytes == mib(252), "unexpected usable payload capacity");
    check(plan.existing_link_updates.size() == 2,
          "appending one extent should update MBR prev and old tail next");
    check(plan.extents[0].prev_lba == ps2hdd::apa::kAllocationChunkSectors,
          "planned extent prev link should point to old tail");
    check(plan.extents[0].next_lba == 0, "planned tail next link should terminate at MBR");
}

void test_overhead_can_force_an_extra_chunk()
{
    const auto scan = make_scan({{0, 1}, {1, 1}});
    const auto plan = ps2hdd::apa::plan_hdl_allocation(scan, disk_bytes(64), mib(253));

    check(plan.ok, "payload just above two-chunk usable capacity should still fit");
    check(plan.extents.size() == 2, "overhead should force a third chunk / second extent");
    check(plan.allocated_bytes == mib(384), "three chunks should allocate 384 MiB");
    check(plan.overhead_bytes == mib(5), "two-part HDL overhead should be 5 MiB");
    check(plan.usable_payload_bytes == mib(379), "unexpected capacity after overhead");
    check(plan.free_chunks_before - plan.free_chunks_after == 3,
          "planner should consume exactly three chunks");
}

void test_fragmented_space_remains_multiple_extents()
{
    const auto scan = make_scan({{0, 1}, {1, 1}, {3, 1}, {5, 1}, {7, 1}});
    const auto plan = ps2hdd::apa::plan_hdl_allocation(scan, disk_bytes(64), mib(300));

    check(plan.ok, "fragmented allocation should succeed when enough chunks exist");
    check(plan.extents.size() >= 3, "separated free chunks must not be folded across occupied chunks");
    for (std::size_t i = 1; i < plan.extents.size(); ++i) {
        check(plan.extents[i - 1].start_lba < plan.extents[i].start_lba,
              "planned extents should stay in physical-LBA order");
        check(plan.extents[i].start_lba % plan.extents[i].length_sectors == 0,
              "every planned sub extent must be self-aligned");
    }
}

void test_noncanonical_chain_is_refused()
{
    auto scan = make_scan({{0, 1}, {1, 1}});
    scan.partitions[1].prev_lba = 0x1234;
    const auto plan = ps2hdd::apa::plan_hdl_allocation(scan, disk_bytes(64), mib(64));

    check(!plan.ok, "planner must refuse a noncanonical APA chain");
    check(plan.error.find("canonical") != std::string::npos,
          "noncanonical chain error should explain the refusal");
}

void test_unaligned_existing_extent_is_refused()
{
    auto scan = make_scan({{0, 1}, {1, 1}});
    scan.partitions[1].length_sectors -= 1;
    scan.partitions[1].total_sectors = scan.partitions[1].length_sectors;
    const auto plan = ps2hdd::apa::plan_hdl_allocation(scan, disk_bytes(64), mib(64));

    check(!plan.ok, "planner must refuse unaligned existing APA extents");
    check(plan.error.find("128 MiB") != std::string::npos,
          "unaligned error should identify APA chunk alignment");
}

void test_existing_extent_must_be_aligned_to_its_own_length()
{
    const auto scan = make_scan({{0, 1}, {2, 4}});
    const auto plan = ps2hdd::apa::plan_hdl_allocation(scan, disk_bytes(64), mib(64));

    check(!plan.ok, "planner must refuse an APA extent whose start is not aligned to its length");
    check(plan.error.find("own allocation length") != std::string::npos,
          "self-alignment refusal should explain the exact invariant");
}

void test_fragmentation_can_hit_hdl_part_limit()
{
    std::vector<std::pair<std::uint32_t, std::uint32_t>> occupied;
    occupied.push_back({0, 1});
    // Occupy every odd chunk, leaving isolated even chunks. No pair can fold.
    for (std::uint32_t chunk = 1; chunk < 200; chunk += 2) {
        occupied.push_back({chunk, 1});
    }
    const auto scan = make_scan(occupied);
    const auto plan = ps2hdd::apa::plan_hdl_allocation(scan, disk_bytes(200), mib(9000));

    check(!plan.ok, "extreme fragmentation should exceed the HDL part limit");
    check(plan.error.find("64 sub-partition") != std::string::npos,
          "part-limit refusal should be explicit");
}

void test_zero_payload_is_refused()
{
    const auto scan = make_scan({{0, 1}});
    const auto plan = ps2hdd::apa::plan_hdl_allocation(scan, disk_bytes(64), 0);
    check(!plan.ok, "zero-byte HDL payload must be refused");
}

} // namespace

int main()
{
    try {
        test_contiguous_chunks_fold_into_aligned_extent();
        test_overhead_can_force_an_extra_chunk();
        test_fragmented_space_remains_multiple_extents();
        test_noncanonical_chain_is_refused();
        test_unaligned_existing_extent_is_refused();
        test_existing_extent_must_be_aligned_to_its_own_length();
        test_fragmentation_can_hit_hdl_part_limit();
        test_zero_payload_is_refused();
        std::cout << "APA allocation planner tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "APA allocation planner tests failed: " << error.what() << '\n';
        return 1;
    }
}
