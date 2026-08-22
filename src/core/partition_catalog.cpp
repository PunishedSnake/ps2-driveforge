#include "ps2hdd/partition_catalog.hpp"

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

} // namespace ps2hdd
