#include "ps2hdd/apa_allocation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ps2hdd::apa {
namespace {

inline constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

struct ChunkRun {
    std::uint32_t first{};
    std::uint32_t count{};
};

struct ChainNode {
    std::uint32_t start_lba{};
    const Partition* existing{};
    std::size_t planned_index{std::numeric_limits<std::size_t>::max()};
};

[[nodiscard]] std::uint64_t ceil_div(std::uint64_t value, std::uint64_t divisor) noexcept
{
    return value / divisor + (value % divisor != 0 ? 1ULL : 0ULL);
}

[[nodiscard]] std::vector<ChunkRun> make_runs(const std::vector<std::uint32_t>& selected,
                                              std::uint32_t max_run_chunks)
{
    std::vector<ChunkRun> runs;
    runs.reserve(selected.size());
    for (const auto chunk : selected) {
        runs.push_back({chunk, 1});
    }

    bool merged = false;
    do {
        merged = false;
        for (std::size_t i = 0; i + 1 < runs.size(); ++i) {
            auto& left = runs[i];
            const auto& right = runs[i + 1];
            const std::uint32_t combined = left.count * 2U;
            const bool same_size = left.count == right.count;
            const bool adjacent = left.first + left.count == right.first;
            const bool aligned = combined != 0 && left.first % combined == 0;
            if (same_size && adjacent && aligned && combined <= max_run_chunks) {
                left.count = combined;
                runs.erase(runs.begin() + static_cast<std::ptrdiff_t>(i + 1));
                merged = true;
                break;
            }
        }
    } while (merged);

    return runs;
}

[[nodiscard]] bool validate_existing_chain(const std::vector<const Partition*>& sorted,
                                           std::string& error)
{
    if (sorted.empty() || sorted.front()->start_lba != 0 || sorted.front()->type != kTypeMbr) {
        error = "APA allocation requires the MBR partition at LBA 0";
        return false;
    }

    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const auto* current = sorted[i];
        const auto expected_prev = i == 0 ? sorted.back()->start_lba : sorted[i - 1]->start_lba;
        const auto expected_next = i + 1 < sorted.size() ? sorted[i + 1]->start_lba : 0U;
        if (current->prev_lba != expected_prev || current->next_lba != expected_next) {
            error = "APA linked list is not in canonical physical-LBA order; refusing allocation planning";
            return false;
        }
    }
    return true;
}

} // namespace

AllocationPlan plan_hdl_allocation(const ScanResult& scan,
                                   std::uint64_t device_size_bytes,
                                   std::uint64_t payload_bytes)
{
    AllocationPlan plan;
    plan.requested_payload_bytes = payload_bytes;

    if (!scan.ok()) {
        plan.error = "APA allocation requires a clean validated scan";
        return plan;
    }
    if (payload_bytes == 0) {
        plan.error = "HDL payload size must be greater than zero";
        return plan;
    }
    if (device_size_bytes < static_cast<std::uint64_t>(kAllocationChunkSectors) * kSectorSize) {
        plan.error = "Backing device is smaller than one 128 MiB APA allocation chunk";
        return plan;
    }
    if (device_size_bytes % kSectorSize != 0) {
        plan.error = "Backing device size is not aligned to 512-byte sectors";
        return plan;
    }

    const std::uint64_t total_chunks64 =
        device_size_bytes / (static_cast<std::uint64_t>(kAllocationChunkSectors) * kSectorSize);
    if (total_chunks64 == 0 || total_chunks64 > std::numeric_limits<std::uint32_t>::max()) {
        plan.error = "APA allocation chunk count is outside the supported planner range";
        return plan;
    }
    plan.total_chunks = static_cast<std::uint32_t>(total_chunks64);

    std::vector<const Partition*> sorted;
    sorted.reserve(scan.partitions.size());
    for (const auto& partition : scan.partitions) {
        sorted.push_back(&partition);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Partition* left, const Partition* right) {
        return left->start_lba < right->start_lba;
    });

    if (!validate_existing_chain(sorted, plan.error)) {
        return plan;
    }

    std::vector<bool> occupied(plan.total_chunks, false);
    for (const auto* partition : sorted) {
        if (partition->length_sectors == 0) {
            plan.error = "APA partition has zero length";
            return plan;
        }
        if (partition->start_lba % kAllocationChunkSectors != 0 ||
            partition->length_sectors % kAllocationChunkSectors != 0) {
            plan.error = "APA partition is not aligned to 128 MiB allocation chunks";
            return plan;
        }

        const std::uint64_t first = partition->start_lba / kAllocationChunkSectors;
        const std::uint64_t count = partition->length_sectors / kAllocationChunkSectors;
        if (first >= plan.total_chunks || count > plan.total_chunks - first) {
            plan.error = "APA partition chunk range extends outside the plannable device area";
            return plan;
        }
        for (std::uint64_t chunk = first; chunk < first + count; ++chunk) {
            if (occupied[static_cast<std::size_t>(chunk)]) {
                plan.error = "APA partitions overlap in the 128 MiB allocation map";
                return plan;
            }
            occupied[static_cast<std::size_t>(chunk)] = true;
        }
    }

    std::vector<std::uint32_t> free_chunks;
    free_chunks.reserve(plan.total_chunks);
    for (std::uint32_t chunk = 0; chunk < plan.total_chunks; ++chunk) {
        if (!occupied[chunk]) {
            free_chunks.push_back(chunk);
        }
    }
    plan.free_chunks_before = static_cast<std::uint32_t>(free_chunks.size());
    if (free_chunks.empty()) {
        plan.error = "No free 128 MiB APA chunks are available";
        return plan;
    }

    const std::uint64_t requested_mib = ceil_div(payload_bytes, kMiB);
    const std::uint32_t max_run_chunks =
        plan.total_chunks < 32 ? 1U : std::max<std::uint32_t>(1U, plan.total_chunks / 32U);

    std::vector<std::uint32_t> selected;
    std::vector<ChunkRun> runs;
    bool payload_fits = false;
    bool part_limit_fits = false;

    for (const auto chunk : free_chunks) {
        selected.push_back(chunk);
        runs = make_runs(selected, max_run_chunks);

        const std::uint64_t allocated_mib =
            static_cast<std::uint64_t>(selected.size()) * kAllocationChunkMiB;
        const std::uint64_t overhead_mib = 3ULL + runs.size();
        payload_fits = allocated_mib >= requested_mib + overhead_mib;
        part_limit_fits = runs.size() <= kMaxSub + 1ULL;
        if (payload_fits && part_limit_fits) {
            break;
        }
    }

    if (!payload_fits) {
        plan.error = "Free APA chunks cannot hold the HDL payload plus metadata overhead";
        return plan;
    }
    if (!part_limit_fits) {
        plan.error = "HDL allocation would exceed the 1 main + 64 sub-partition limit";
        return plan;
    }

    plan.extents.reserve(runs.size());
    for (const auto& run : runs) {
        const std::uint64_t start = static_cast<std::uint64_t>(run.first) * kAllocationChunkSectors;
        const std::uint64_t length = static_cast<std::uint64_t>(run.count) * kAllocationChunkSectors;
        if (start > std::numeric_limits<std::uint32_t>::max() ||
            length > std::numeric_limits<std::uint32_t>::max()) {
            plan.error = "Planned APA extent exceeds 32-bit on-disk fields";
            plan.extents.clear();
            return plan;
        }
        plan.extents.push_back({static_cast<std::uint32_t>(start),
                                static_cast<std::uint32_t>(length), 0, 0});
    }

    plan.allocated_bytes =
        static_cast<std::uint64_t>(selected.size()) * kAllocationChunkMiB * kMiB;
    plan.overhead_bytes = (3ULL + plan.extents.size()) * kMiB;
    plan.usable_payload_bytes = plan.allocated_bytes - plan.overhead_bytes;
    plan.free_chunks_after =
        plan.free_chunks_before - static_cast<std::uint32_t>(selected.size());

    std::vector<ChainNode> final_chain;
    final_chain.reserve(sorted.size() + plan.extents.size());
    for (const auto* partition : sorted) {
        final_chain.push_back({partition->start_lba, partition,
                               std::numeric_limits<std::size_t>::max()});
    }
    for (std::size_t i = 0; i < plan.extents.size(); ++i) {
        final_chain.push_back({plan.extents[i].start_lba, nullptr, i});
    }
    std::sort(final_chain.begin(), final_chain.end(), [](const ChainNode& left, const ChainNode& right) {
        return left.start_lba < right.start_lba;
    });

    for (std::size_t i = 0; i < final_chain.size(); ++i) {
        const auto new_prev = i == 0 ? final_chain.back().start_lba : final_chain[i - 1].start_lba;
        const auto new_next = i + 1 < final_chain.size() ? final_chain[i + 1].start_lba : 0U;
        if (final_chain[i].existing != nullptr) {
            const auto* existing = final_chain[i].existing;
            if (existing->prev_lba != new_prev || existing->next_lba != new_next) {
                plan.existing_link_updates.push_back({existing->start_lba,
                                                      existing->prev_lba,
                                                      existing->next_lba,
                                                      new_prev,
                                                      new_next});
            }
        } else {
            auto& extent = plan.extents[final_chain[i].planned_index];
            extent.prev_lba = new_prev;
            extent.next_lba = new_next;
        }
    }

    if (plan.usable_payload_bytes < payload_bytes) {
        plan.error = "Internal planner error: computed HDL payload capacity is insufficient";
        plan.extents.clear();
        plan.existing_link_updates.clear();
        return plan;
    }

    plan.ok = true;
    return plan;
}

} // namespace ps2hdd::apa
