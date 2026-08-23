#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd::opl {

struct PartitionEvidence {
    std::string partition_id;
    int score{};
    bool pfs_valid{};
    std::vector<std::string> reasons;
};

struct PartitionResolution {
    bool ok{};
    std::string partition_id;
    std::size_t partition_index{};
    std::vector<PartitionEvidence> candidates;
    std::string error;
};

// Read-only resolver for the OPL data PFS partition. A configured partition ID
// is authoritative when it names a non-sub PFS partition. Without one, valid
// PFS candidates are scored using the APA ID and familiar root directories such
// as ART/CFG/CHT/VMC/THM/APPS. Ambiguous top scores are refused instead of
// silently writing to whichever partition happened to be scanned first.
[[nodiscard]] PartitionResolution resolve_data_partition(
    BlockDevice& device,
    const apa::ScanResult& scan,
    std::optional<std::string_view> configured_partition = std::nullopt);

} // namespace ps2hdd::opl
