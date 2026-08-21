#pragma once

#include "ps2hdd/hdl.hpp"
#include "ps2hdd/partition_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
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
// on the owning UI thread without rebuilding/rescanning the catalog.
class ManagementModel final {
public:
    explicit ManagementModel(PartitionCatalog catalog)
    {
        rows_.reserve(catalog.entries.size());
        for (auto& partition : catalog.entries) {
            ManagementRow row;
            row.partition = std::move(partition);
            if (!row.partition.is_sub && row.partition.kind == PartitionCatalogKind::hdl) {
                row.hdl_state = EnrichmentState::pending;
                hdl_rows_by_lba_.emplace(row.partition.start_lba, rows_.size());
                ++progress_.total_hdl;
                ++progress_.pending_hdl;
            }
            rows_.emplace_back(std::move(row));
        }
    }

    [[nodiscard]] const std::vector<ManagementRow>& rows() const noexcept { return rows_; }
    [[nodiscard]] ManagementProgress progress() const noexcept { return progress_; }

    // Returns the row index updated by this result, or nullopt if the result no
    // longer belongs to this model (for example after a source/session change).
    [[nodiscard]] std::optional<std::size_t> apply_hdl_result(const hdl::GameResult& result)
    {
        const auto found = hdl_rows_by_lba_.find(result.game.start_lba);
        if (found == hdl_rows_by_lba_.end()) {
            return std::nullopt;
        }

        const auto index = found->second;
        auto& row = rows_[index];
        if (row.hdl_state == EnrichmentState::pending && progress_.pending_hdl != 0) {
            --progress_.pending_hdl;
        } else if (row.hdl_state == EnrichmentState::ready && progress_.ready_hdl != 0) {
            --progress_.ready_hdl;
        } else if (row.hdl_state == EnrichmentState::error && progress_.failed_hdl != 0) {
            --progress_.failed_hdl;
        }

        row.hdl_game.reset();
        row.enrichment_error.clear();
        if (result.ok) {
            row.hdl_state = EnrichmentState::ready;
            row.hdl_game = result.game;
            ++progress_.ready_hdl;
        } else {
            row.hdl_state = EnrichmentState::error;
            row.enrichment_error = result.error;
            ++progress_.failed_hdl;
        }
        return index;
    }

private:
    std::vector<ManagementRow> rows_;
    std::unordered_map<std::uint32_t, std::size_t> hdl_rows_by_lba_;
    ManagementProgress progress_{};
};

} // namespace ps2hdd
