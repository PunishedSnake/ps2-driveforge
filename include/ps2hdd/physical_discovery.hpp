#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <vector>

namespace ps2hdd {

struct PhysicalDriveProbe {
    unsigned index{};
    bool opened{};
    bool apa_detected{};
    bool apa_clean{};
    std::uint64_t size_bytes{};
    std::uint32_t apa_version{};
    std::size_t partition_count{};
    std::string note;
};

// Probe raw Windows disks read-only and classify APA candidates. This never
// requests GENERIC_WRITE and is safe to use as a discovery step before the user
// chooses a disk explicitly.
[[nodiscard]] std::vector<PhysicalDriveProbe> discover_physical_drives(unsigned max_index = 32);

} // namespace ps2hdd

#endif
