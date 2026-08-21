#pragma once

#ifdef _WIN32

#include "ps2hdd/block_device.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ps2hdd {

struct PhysicalDriveProbe {
    unsigned index{};
    std::string friendly_name;
    bool opened{};
    bool access_denied{};
    bool apa_detected{};
    bool apa_clean{};
    std::uint64_t size_bytes{};
    std::uint32_t apa_version{};
    std::size_t partition_count{};      // all APA headers, including SUB entries
    std::size_t main_partition_count{};
    StorageCharacteristics storage{};
    std::string note;
};

// Enumerate real Windows disk interfaces through SetupAPI, map each interface to
// its STORAGE_DEVICE_NUMBER, then probe the corresponding PhysicalDrive read-only.
// max_index is retained only as an optional CLI compatibility filter; discovery
// never guesses or iterates nonexistent PhysicalDrive numbers.
[[nodiscard]] std::vector<PhysicalDriveProbe> discover_physical_drives(
    unsigned max_index = std::numeric_limits<unsigned>::max());

} // namespace ps2hdd

#endif
