#pragma once

#include "ps2hdd/apa_allocation.hpp"
#include "ps2hdd/block_device.hpp"
#include "ps2hdd/ps2_iso.hpp"

#include <cstdint>
#include <string>

namespace ps2hdd::hdl {

struct InstallPlan {
    bool ok{};
    std::string error;
    std::string title;
    std::string partition_id;
    iso::GameSourceInfo source;
    apa::AllocationPlan allocation;
};

// Build a canonical HDL APA partition identifier from a PS2 startup ID and
// display title. The result follows the de-facto PP.XXXX-00000..TITLE layout
// while remaining bounded to the 32-byte APA ID field.
[[nodiscard]] std::string make_partition_id(const std::string& startup,
                                            const std::string& title,
                                            bool hidden = false);

// Read-only install preflight: inspect SYSTEM.CNF in the game ISO, derive the
// partition ID, and run the zero-write APA allocation planner. This is the
// object the future UI should show before any mutation capability is opened.
[[nodiscard]] InstallPlan plan_install(const apa::ScanResult& disk_scan,
                                       std::uint64_t disk_size_bytes,
                                       BlockDevice& game_iso,
                                       std::string title,
                                       bool hidden = false);

} // namespace ps2hdd::hdl
