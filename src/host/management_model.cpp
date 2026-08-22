#include "ps2hdd/management_model.hpp"

#include <utility>

namespace ps2hdd {

ManagementModel::ManagementModel(PartitionCatalog catalog)
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

std::optional<std::size_t> ManagementModel::apply_hdl_result(const hdl::GameResult& result)
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

} // namespace ps2hdd
