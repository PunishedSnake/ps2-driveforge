#include "ps2hdd/management_model.hpp"

#include <algorithm>
#include <unordered_set>
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
        }
        rows_.emplace_back(std::move(row));
    }
    rebuild_hdl_index_and_progress();
}

void ManagementModel::rebuild_hdl_index_and_progress()
{
    hdl_rows_by_lba_.clear();
    progress_ = {};
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_[index];
        if (row.partition.is_sub || row.partition.kind != PartitionCatalogKind::hdl) {
            continue;
        }
        hdl_rows_by_lba_.emplace(row.partition.start_lba, index);
        ++progress_.total_hdl;
        switch (row.hdl_state) {
        case EnrichmentState::pending:
            ++progress_.pending_hdl;
            break;
        case EnrichmentState::ready:
            ++progress_.ready_hdl;
            break;
        case EnrichmentState::error:
            ++progress_.failed_hdl;
            break;
        case EnrichmentState::not_applicable:
            break;
        }
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

bool ManagementModel::apply_partition_removal(const apa::RemovePlan& plan)
{
    if (!plan.ok || plan.removed_lbas.empty()) {
        return false;
    }

    const std::unordered_set<std::uint32_t> removed(plan.removed_lbas.begin(),
                                                    plan.removed_lbas.end());
    const auto old_size = rows_.size();
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [&](const ManagementRow& row) {
                    return removed.contains(row.partition.start_lba);
                }),
                rows_.end());
    if (rows_.size() == old_size) {
        return false;
    }

    // Reindexing a vector in RAM is effectively free compared with touching the
    // device again. Crucially, surviving HDL enrichment payloads stay attached
    // to their rows and no parser/background job is restarted.
    rebuild_hdl_index_and_progress();
    return true;
}

} // namespace ps2hdd
