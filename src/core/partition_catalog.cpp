#include "ps2hdd/partition_catalog.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace ps2hdd {

PartitionCatalogKind catalog_kind(std::uint16_t type) noexcept
{
    switch (type) {
    case apa::kTypeMbr:
        return PartitionCatalogKind::mbr;
    case apa::kTypePfs:
        return PartitionCatalogKind::pfs;
    case apa::kTypeHdl:
        return PartitionCatalogKind::hdl;
    case apa::kTypeFree:
        return PartitionCatalogKind::free_space;
    default:
        return PartitionCatalogKind::other;
    }
}

PartitionCatalog build_partition_catalog(const apa::ScanResult& scan,
                                         bool include_sub_partitions)
{
    PartitionCatalog catalog;
    catalog.entries.reserve(scan.partitions.size());

    for (const auto& partition : scan.partitions) {
        const bool is_sub = partition.is_sub();
        if (is_sub) {
            ++catalog.sub_partitions;
            if (!include_sub_partitions) {
                continue;
            }
        } else {
            ++catalog.main_partitions;
        }

        PartitionCatalogEntry entry;
        entry.id = partition.id;
        entry.kind = catalog_kind(partition.type);
        entry.raw_type = partition.type;
        entry.flags = partition.flags;
        entry.start_lba = partition.start_lba;
        entry.main_lba = partition.main_lba;
        entry.number = partition.number;
        entry.sub_count = partition.sub_count;
        entry.extent_size_bytes =
            static_cast<std::uint64_t>(partition.length_sectors) * apa::kSectorSize;
        entry.size_bytes = partition.size_bytes();
        entry.is_sub = is_sub;
        catalog.entries.emplace_back(std::move(entry));

        if (is_sub) {
            continue;
        }
        switch (partition.type) {
        case apa::kTypeHdl:
            ++catalog.hdl_partitions;
            catalog.hdl_bytes += partition.size_bytes();
            break;
        case apa::kTypePfs:
            ++catalog.pfs_partitions;
            catalog.pfs_bytes += partition.size_bytes();
            break;
        case apa::kTypeFree:
            catalog.free_bytes += partition.size_bytes();
            break;
        default:
            break;
        }
    }

    return catalog;
}

PartitionCatalogGrouping group_partition_catalog(const PartitionCatalog& catalog)
{
    PartitionCatalogGrouping result;
    result.groups.reserve(catalog.main_partitions);

    std::unordered_map<std::uint32_t, std::size_t> group_by_main_lba;
    group_by_main_lba.reserve(catalog.main_partitions);

    // Preserve the existing main-partition display order. Children are attached
    // only after every main is indexed, so a sub-header may appear before or
    // after its owner in a damaged/non-canonical listing without changing the
    // relationship decision.
    for (const auto& entry : catalog.entries) {
        if (entry.is_sub) {
            continue;
        }
        PartitionCatalogGroup group;
        group.main = entry;
        group.logical_size_bytes = entry.size_bytes;
        group.physical_extent_bytes = entry.extent_size_bytes;
        const auto index = result.groups.size();
        result.groups.emplace_back(std::move(group));
        group_by_main_lba.emplace(entry.start_lba, index);
    }

    for (const auto& entry : catalog.entries) {
        if (!entry.is_sub) {
            continue;
        }
        const auto owner = group_by_main_lba.find(entry.main_lba);
        if (owner == group_by_main_lba.end()) {
            result.orphan_sub_partitions.push_back(entry);
            continue;
        }
        auto& group = result.groups[owner->second];
        group.subpartition_bytes += entry.extent_size_bytes;
        group.physical_extent_bytes += entry.extent_size_bytes;
        group.sub_partitions.push_back(entry);
    }

    for (auto& group : result.groups) {
        std::sort(group.sub_partitions.begin(), group.sub_partitions.end(),
                  [](const PartitionCatalogEntry& left,
                     const PartitionCatalogEntry& right) {
                      if (left.number != right.number) {
                          return left.number < right.number;
                      }
                      return left.start_lba < right.start_lba;
                  });
        group.complete = group.sub_partitions.size() == group.main.sub_count;
    }

    std::sort(result.orphan_sub_partitions.begin(),
              result.orphan_sub_partitions.end(),
              [](const PartitionCatalogEntry& left,
                 const PartitionCatalogEntry& right) {
                  return left.start_lba < right.start_lba;
              });
    return result;
}

} // namespace ps2hdd
