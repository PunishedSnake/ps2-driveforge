#pragma once

#include "ps2hdd/apa.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2hdd::apa {

inline constexpr std::uint32_t kAllocationChunkMiB = 128;
inline constexpr std::uint32_t kAllocationChunkSectors =
    kAllocationChunkMiB * 1024U * 1024U / kSectorSize;

struct PlannedExtent {
    std::uint32_t start_lba{};
    std::uint32_t length_sectors{};
    std::uint32_t prev_lba{};
    std::uint32_t next_lba{};

    [[nodiscard]] std::uint32_t chunks() const noexcept
    {
        return length_sectors / kAllocationChunkSectors;
    }
};

struct LinkUpdate {
    std::uint32_t header_lba{};
    std::uint32_t old_prev_lba{};
    std::uint32_t old_next_lba{};
    std::uint32_t new_prev_lba{};
    std::uint32_t new_next_lba{};
};

struct AllocationPlan {
    bool ok{};
    std::string error;
    std::uint64_t requested_payload_bytes{};
    std::uint64_t allocated_bytes{};
    std::uint64_t overhead_bytes{};
    std::uint64_t usable_payload_bytes{};
    std::uint32_t total_chunks{};
    std::uint32_t free_chunks_before{};
    std::uint32_t free_chunks_after{};
    std::vector<PlannedExtent> extents;
    std::vector<LinkUpdate> existing_link_updates;

    [[nodiscard]] std::uint32_t main_start_lba() const noexcept
    {
        return extents.empty() ? 0 : extents.front().start_lba;
    }

    [[nodiscard]] std::size_t sub_count() const noexcept
    {
        return extents.empty() ? 0 : extents.size() - 1;
    }
};

// Pure Frieren F3 planner. It derives a conservative 128 MiB APA chunk map from
// an already validated scan, chooses free chunks for an HDL payload, folds
// suitably aligned neighbouring chunks into larger extents, and predicts the
// existing linked-list headers whose prev/next fields would change.
//
// No source I/O is performed and no Header is serialized here. The result is a
// proposal that later format code must turn into concrete main/sub headers.
[[nodiscard]] AllocationPlan plan_hdl_allocation(const ScanResult& scan,
                                                 std::uint64_t device_size_bytes,
                                                 std::uint64_t payload_bytes);

} // namespace ps2hdd::apa
