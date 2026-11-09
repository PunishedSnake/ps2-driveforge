#pragma once

#include "ps2hdd/block_device.hpp"

#include <cstdint>
#include <string>

namespace ps2hdd::iso {

inline constexpr std::uint32_t kSectorSize = 2048;
inline constexpr std::uint32_t kPrimaryVolumeDescriptorLba = 16;

struct GameSourceInfo {
    std::string startup;
    std::string system_cnf;
    std::uint64_t image_bytes{};
};

struct GameSourceResult {
    bool ok{};
    std::string error;
    GameSourceInfo game;
};

// Inspect a conventional PS2 ISO9660 image without mounting it. Frieren only
// needs the root SYSTEM.CNF at this stage; Joliet/Rock Ridge are irrelevant to
// the BOOT2 startup identifier and are deliberately not a dependency.
[[nodiscard]] GameSourceResult inspect_ps2_iso(BlockDevice& device);

} // namespace ps2hdd::iso
