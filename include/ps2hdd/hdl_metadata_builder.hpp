#pragma once

#include "ps2hdd/apa_allocation.hpp"
#include "ps2hdd/hdl.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2hdd::hdl {

struct PayloadExtent {
    std::uint64_t source_offset_bytes{};
    std::uint64_t device_offset_bytes{};
    std::uint64_t bytes{};
    std::size_t apa_extent_index{};
};

struct MetadataBuildRequest {
    std::string title;
    std::string startup;
    std::uint8_t compat_flags{};
    std::uint16_t dma{};
    std::uint32_t layer_break{};
    MediaType media{MediaType::dvd};
    std::uint64_t payload_bytes{};
};

struct MetadataBuildResult {
    bool ok{};
    std::string error;
    std::array<std::byte, kMetadataBytes> metadata{};
    std::vector<PayloadExtent> payload_extents;
};

// Build the native 0xDEADFEED metadata window and the exact game-payload copy
// map for an already approved APA AllocationPlan. The main extent reserves 4
// MiB before game data; each sub extent reserves 1 MiB. No device I/O occurs.
[[nodiscard]] MetadataBuildResult build_install_metadata(
    const apa::AllocationPlan& allocation,
    const MetadataBuildRequest& request);

} // namespace ps2hdd::hdl
