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

[[nodiscard]] PartitionCatalogKind catalog_kind(std::uint16_t type) noexcept;

// Build the complete HDD-manager list from the already validated APA scan.
// This function performs no device I/O and deliberately does not probe PFS or
// HDL payload metadata. Expensive enrichment belongs to lazy/background jobs.
[[nodiscard]] PartitionCatalog build_partition_catalog(const apa::ScanResult& scan,
                                                        bool include_sub_partitions = true);

} // namespace ps2hdd
