#pragma once

#include "ps2hdd/apa_remove.hpp"
#include "ps2hdd/hdl.hpp"
#include "ps2hdd/partition_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2hdd {

enum class EnrichmentState {
    not_applicable,
    pending,
    ready,
    error,
};

struct ManagementRow {
    PartitionCatalogEntry partition;
    EnrichmentState hdl_state{EnrichmentState::not_applicable};
    std::optional<hdl::GameInfo> hdl_game;
    std::string enrichment_error;
};

struct ManagementProgress {
    std::size_t total_hdl{};
    std::size_t pending_hdl{};
    std::size_t ready_hdl{};
    std::size_t failed_hdl{};
};

// Frontend-neutral HDD Manager state. Construction is a pure in-memory transform
// of PartitionCatalog and therefore never touches the disk. HDL rows start as
// `pending`; a background enrichment worker can later apply results one at a time
// without rebuilding or rescanning the catalog.
class ManagementModel final {
public:
    explicit ManagementModel(PartitionCatalog catalog);

    [[nodiscard]] const std::vector<ManagementRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] ManagementProgress progress() const noexcept { return progress_; }

    // Returns the row index updated by this result, or nullopt if the result no
    // longer belongs to this model (for example after a source/session change).
    [[nodiscard]] std::optional<std::size_t> apply_hdl_result(const hdl::GameResult& result);

    // Apply a successfully planned/committed APA removal directly to the current
    // in-memory model. This is intentionally O(rows) RAM work with zero device
    // I/O and zero HDL re-enrichment. It is the fast path that prevents deleting
    // one game from turning into a complete HDD list rebuild.
    [[nodiscard]] bool apply_partition_removal(const apa::RemovePlan& plan);

private:
    void rebuild_hdl_index_and_progress();

    std::vector<ManagementRow> rows_;
    std::unordered_map<std::uint32_t, std::size_t> hdl_rows_by_lba_;
    ManagementProgress progress_{};
};

} // namespace ps2hdd
