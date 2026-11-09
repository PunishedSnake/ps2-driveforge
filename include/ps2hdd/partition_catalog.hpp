#pragma once

#include "ps2hdd/apa.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
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

    // Physical bytes owned by this header's own APA extent only.
    std::uint64_t extent_size_bytes{};

    // Logical bytes represented by this entry. For a main APA partition the
    // Reader already folds the extents listed in subs[] into total_sectors, so
    // this includes its sub-partitions. For a sub-partition it equals
    // extent_size_bytes. Frontends must not add child sizes to this value again.
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

// Frontend-neutral main+children representation. `logical_size_bytes` is the
// already aggregated size exposed by the main header. `physical_extent_bytes`
// is independently reconstructed from the main extent plus discovered child
// headers and is useful as a consistency/diagnostic signal.
struct PartitionCatalogGroup {
    PartitionCatalogEntry main;
    std::vector<PartitionCatalogEntry> sub_partitions;
    std::uint64_t logical_size_bytes{};
    std::uint64_t physical_extent_bytes{};
    std::uint64_t subpartition_bytes{};
    bool complete{};

    [[nodiscard]] bool is_hdl_game() const noexcept
    {
        return main.kind == PartitionCatalogKind::hdl;
    }
};

struct PartitionCatalogGrouping {
    std::vector<PartitionCatalogGroup> groups;
    std::vector<PartitionCatalogEntry> orphan_sub_partitions;
};

[[nodiscard]] PartitionCatalogKind catalog_kind(std::uint16_t type) noexcept;

// Build the complete HDD-manager list from the already validated APA scan.
// This function performs no device I/O and deliberately does not probe PFS or
// HDL payload metadata. Expensive enrichment belongs to lazy/background jobs.
[[nodiscard]] PartitionCatalog build_partition_catalog(const apa::ScanResult& scan,
                                                        bool include_sub_partitions = true);

// Attach APA sub-partitions to their authoritative main header by main_lba,
// never by display ordering or partition name. Unknown/empty child IDs are
// therefore naturally folded under their owning game/PFS volume. Orphans are
// retained separately instead of being silently attached to a nearby main.
[[nodiscard]] PartitionCatalogGrouping group_partition_catalog(
    const PartitionCatalog& catalog);

} // namespace ps2hdd
