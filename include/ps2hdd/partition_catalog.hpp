#pragma once

#include "ps2hdd/apa.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd {

enum class PartitionCatalogKind {
    mbr,
    pfs,
    hdl,
    free_space,
    other,
};

struct PartitionCatalogEntry {
    std::string id;
    PartitionCatalogKind kind{PartitionCatalogKind::other};
    std::uint16_t raw_type{};
    std::uint16_t flags{};
    std::uint32_t start_lba{};
    std::uint32_t main_lba{};
    std::uint32_t number{};
    std::uint32_t sub_count{};
    std::uint64_t size_bytes{};
    bool is_sub{};
};

struct PartitionCatalog {
    std::vector<PartitionCatalogEntry> entries;
    std::size_t main_partitions{};
    std::size_t sub_partitions{};
    std::size_t hdl_partitions{};
    std::size_t pfs_partitions{};
    std::uint64_t hdl_bytes{};
    std::uint64_t pfs_bytes{};
    std::uint64_t free_bytes{};
};

[[nodiscard]] inline PartitionCatalogKind catalog_kind(std::uint16_t type) noexcept
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

// Build the complete HDD-manager list from the already validated APA scan.
// This function performs no device I/O and deliberately does not probe PFS or
// HDL payload metadata. Expensive enrichment belongs to lazy/background jobs.
[[nodiscard]] inline PartitionCatalog build_partition_catalog(const apa::ScanResult& scan,
                                                               bool include_sub_partitions = true)
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
