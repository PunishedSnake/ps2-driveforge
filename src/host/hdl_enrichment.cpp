#include "ps2hdd/hdl_enrichment.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace ps2hdd {
namespace {

std::vector<const apa::Partition*> hdl_mains_by_lba(const apa::ScanResult& scan)
{
    std::vector<const apa::Partition*> partitions;
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.type == apa::kTypeHdl) {
            partitions.push_back(&partition);
        }
    }
    std::sort(partitions.begin(), partitions.end(), [](const auto* left, const auto* right) {
        return left->start_lba < right->start_lba;
    });
    return partitions;
}

} // namespace

HdlEnrichmentPolicy choose_hdl_enrichment_policy(
    const StorageCharacteristics& storage, std::size_t candidates,
    HdlEnrichmentOptions options) noexcept
{
    HdlEnrichmentPolicy policy;
    if (candidates == 0) {
        policy.max_in_flight = 0;
        return policy;
    }

    if (options.max_in_flight_override != 0) {
        policy.max_in_flight = std::min(options.max_in_flight_override, candidates);
        return policy;
    }

    // Rotational and unknown devices start at queue depth 1. This keeps seek
    // behavior predictable even when a SATA/USB bridge hides media identity.
    // Explicitly non-rotating media may exploit independent OVERLAPPED reads;
    // the conservative cap avoids turning metadata enrichment into a stress test.
    if (storage.media_class == StorageMediaClass::solid_state) {
        policy.max_in_flight = std::min<std::size_t>(4, candidates);
    } else {
        policy.max_in_flight = 1;
    }
    return policy;
}

hdl::CatalogResult enrich_hdl_catalog(BlockDevice& device, const apa::ScanResult& scan,
                                      hdl::ProgressCallback progress,
                                      std::stop_token stop,
                                      HdlEnrichmentOptions options)
{
    const auto partitions = hdl_mains_by_lba(scan);
    const auto policy = choose_hdl_enrichment_policy(
        device.storage_characteristics(), partitions.size(), options);

    hdl::CatalogResult catalog;
    catalog.total_candidates = partitions.size();
    catalog.entries.reserve(partitions.size());
    if (partitions.empty()) {
        return catalog;
    }

    if (policy.max_in_flight <= 1) {
        return hdl::read_catalog(device, scan, std::move(progress), stop);
    }

    std::vector<std::optional<hdl::GameResult>> results(partitions.size());
    std::atomic<std::size_t> next{};
    std::atomic<std::size_t> completed{};
    std::mutex progress_mutex;

    const auto worker_count = std::min(policy.max_in_flight, partitions.size());
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            while (!stop.stop_requested()) {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= partitions.size()) {
                    break;
                }

                auto game = hdl::read_game_info(device, *partitions[index]);
                results[index] = std::move(game);
                const auto done = completed.fetch_add(1, std::memory_order_relaxed) + 1;

                if (progress) {
                    std::scoped_lock lock(progress_mutex);
                    progress(done, partitions.size(), *results[index]);
                }
            }
        });
    }
    // jthread joins when workers leave scope.
    workers.clear();

    for (auto& result : results) {
        if (!result) {
            continue;
        }
        if (result->ok) {
            ++catalog.readable;
        } else {
            ++catalog.unreadable;
        }
        catalog.entries.emplace_back(std::move(*result));
    }
    catalog.cancelled = catalog.entries.size() != catalog.total_candidates;
    return catalog;
}

} // namespace ps2hdd
