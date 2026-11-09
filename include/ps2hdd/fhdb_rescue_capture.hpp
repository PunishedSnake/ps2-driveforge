#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/fhdb_rescue.hpp"

#include <cstdint>
#include <string>

namespace ps2hdd::fhdb {

// FHDB Manager reserves the program area inside __mbr from sector 0x2000 and
// caps bootstrap payload handling at 4 MiB. Keep these values here rather than
// sprinkling literals through physical-drive frontends. A future installer for
// FHDB/HDD-OSD/HOSD/PSBBN must prove its payload fits the same geometry before
// it earns write access.
inline constexpr std::uint32_t kBootstrapProgramStartSector = 0x2000U;
inline constexpr std::uint32_t kBootstrapPayloadMaxBytes = 4U * 1024U * 1024U;

struct RescueCaptureOptions {
    // Diagnostic strings are metadata only. They never participate in locating
    // the payload. Capture follows the live APA osdStart/osdSize pointer exactly
    // because a recovery tool that searches for a more convenient KELF would be
    // rewriting history before the backup even exists.
    std::string romver;
    std::string family;
    std::string confidence;
};

// Capture a canonical PS2HBRC v1 Rescue Capsule directly from a block device.
// This operation is strictly read-only:
//
//   1. read and validate the exact 1024-byte APA master;
//   2. reject conflicting PC/MBR evidence;
//   3. inspect only the live osdStart/osdSize pointer;
//   4. prove that range lies inside the __mbr program area and device bounds;
//   5. read those exact sectors and build the normal shared Rescue Capsule.
//
// A zero/zero pointer produces a valid header-only capsule. A half-empty or
// out-of-range pointer is corruption and fails closed. The builder may preserve
// a non-KELF payload as evidence, but VALID_KELF is set only when the existing
// structural KELF validator proves it.
[[nodiscard]] RescueImageResult capture_rescue_image(
    BlockDevice& disk,
    const RescueCaptureOptions& options = {});

} // namespace ps2hdd::fhdb
