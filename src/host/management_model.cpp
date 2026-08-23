#include "ps2hdd/management_model.hpp"

#include <algorithm>
#include <unordered_map>
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
    rebuild_groups();
    rebuild_hdl_index_and_progress();
}

void ManagementModel::rebuild_groups()
{
    groups_.clear();
    orphan_sub_rows_.clear();

    std::unordered_map<std::uint32_t, std::size_t> group_by_main_lba;
    group_by_main_lba.reserve(rows_.size());

    for (std::size_t row_index = 0; row_index < rows_.size(); ++row_index) {
        const auto& partition = rows_[row_index].partition;
        if (partition.is_sub) {
            continue;
        }

        ManagementGroup group;
        group.main_row = row_index;
        group.logical_size_bytes = partition.size_bytes;
        group.physical_extent_bytes = partition.extent_size_bytes;
        const auto group_index = groups_.size();
        groups_.push_back(std::move(group));
        group_by_main_lba.emplace(partition.start_lba, group_index);
    }

    for (std::size_t row_index = 0; row_index < rows_.size(); ++row_index) {
        const auto& partition = rows_[row_index].partition;
        if (!partition.is_sub) {
            continue;
        }
        const auto owner = group_by_main_lba.find(partition.main_lba);
        if (owner == group_by_main_lba.end()) {
            orphan_sub_rows_.push_back(row_index);
            continue;
        }

        auto& group = groups_[owner->second];
        group.sub_rows.push_back(row_index);
        group.subpartition_bytes += partition.extent_size_bytes;
        group.physical_extent_bytes += partition.extent_size_bytes;
    }

    for (auto& group : groups_) {
        std::sort(group.sub_rows.begin(), group.sub_rows.end(),
                  [&](std::size_t left, std::size_t right) {
                      const auto& a = rows_[left].partition;
                      const auto& b = rows_[right].partition;
                      if (a.number != b.number) {
                          return a.number < b.number;
                      }
                      return a.start_lba < b.start_lba;
                  });
        const auto& main = rows_[group.main_row].partition;
        group.complete = group.sub_rows.size() == main.sub_count;
    }

    std::sort(orphan_sub_rows_.begin(), orphan_sub_rows_.end(),
              [&](std::size_t left, std::size_t right) {
                  return rows_[left].partition.start_lba <
                         rows_[right].partition.start_lba;
              });
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

    // Reindexing vectors in RAM is effectively free compared with touching the
    // device again. Surviving HDL enrichment payloads stay attached to their
    // rows; group ownership is rebuilt from authoritative main_lba relationships.
    rebuild_groups();
    rebuild_hdl_index_and_progress();
    return true;
}

} // namespace ps2hdd
