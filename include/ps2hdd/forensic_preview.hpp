#pragma once

#include "ps2hdd/apa_forensic.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2hdd::forensic {

// Read-only "look before you leap" view of one candidate map: the partition
// chain it would publish, what each touched header changes, and which scanned
// headers it would leave unreachable. Nothing here alters the plan's safety
// classification; it only explains it.
struct PreviewRow {
    std::uint32_t lba{};
    std::string id;
    std::string type;      // apa::type_name
    std::uint64_t bytes{};
    bool sub{};
    unsigned confidence{};
    std::string change;    // "next 0x.. -> 0x.., prev .. -> .." or empty
};

struct MapPreview {
    std::size_t map_index{};
    std::string kind;
    std::string safety;  // automatic-safe / manual-only / blocked
    unsigned confidence{};
    std::vector<PreviewRow> rows;
    std::vector<PreviewRow> orphaned;  // scanned, not in this map
    std::size_t pfs_partitions{};
    std::size_t hdl_games{};
    std::uint64_t mapped_bytes{};
    std::size_t patches{};
};

[[nodiscard]] MapPreview preview(const ScanResult& scan, std::size_t map_index);
[[nodiscard]] std::string render_preview(const MapPreview& preview);

} // namespace ps2hdd::forensic
