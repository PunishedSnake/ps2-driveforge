#pragma once

#include "ps2hdd/hdl.hpp"

#include <cstddef>
#include <stop_token>

namespace ps2hdd {

struct HdlEnrichmentPolicy {
    std::size_t max_in_flight{1};
    bool physical_lba_order{true};
};

struct HdlEnrichmentOptions {
    // 0 = automatic from storage characteristics. Non-zero is reserved for the
    // benchmark/developer path; normal frontends should leave this at zero.
    std::size_t max_in_flight_override{};
};

[[nodiscard]] HdlEnrichmentPolicy choose_hdl_enrichment_policy(
    const StorageCharacteristics& storage, std::size_t candidates,
    HdlEnrichmentOptions options = {}) noexcept;

// Host-level scheduler for independent HDLoader metadata reads. The parser stays
// deterministic and read-only in hdl.hpp; this layer decides whether those
// independent reads should remain seek-friendly serial I/O or execute concurrently.
// Returned entries are always in physical-LBA order regardless of completion order.
// Progress callbacks may run on worker threads when max_in_flight > 1; GUI callers
// must marshal UI mutations to their dispatcher.
[[nodiscard]] hdl::CatalogResult enrich_hdl_catalog(
    BlockDevice& device, const apa::ScanResult& scan,
    hdl::ProgressCallback progress = {}, std::stop_token stop = {},
    HdlEnrichmentOptions options = {});

} // namespace ps2hdd
