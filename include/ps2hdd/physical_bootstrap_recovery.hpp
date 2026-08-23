#pragma once

#ifdef _WIN32

#include "ps2hdd/fhdb_restore.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace ps2hdd {

struct PhysicalBootstrapRestoreOptions {
    // Directory containing HDDRESCUE/HDDMBR/FHDBMBR inputs discovered with the
    // exact FHDB Manager precedence implemented by plan_bootstrap_restore().
    std::filesystem::path artifact_directory;

    // Destination for the mandatory current-master safety snapshot. It may be
    // the same directory as artifact_directory; keeping it explicit prevents a
    // frontend from accidentally restoring without first choosing where fresh
    // before-image evidence is allowed to live.
    std::filesystem::path safety_directory;

    bool lock_mounted_volumes{true};
    bool dismount_locked_volumes{true};
};

struct PhysicalBootstrapRestoreResult {
    bool ok{};
    bool partial{};
    std::string error;
    std::size_t locked_volume_count{};
    bool cold_master_verified{};
    bool cold_payload_verified{};
    fhdb::BootstrapRestoreResult restore;
};

// Apply the existing storage-neutral FHDB bootstrap restore algorithm to a real
// Windows PS2 HDD. The coordinator deliberately owns only endpoint safety:
//
//   PhysicalDrive read-only evidence + normal physical authorization
//       -> immutable FHDB restore plan
//       -> WritablePhysicalDrive identity recheck + volume lease
//       -> apply_bootstrap_restore() payload-first / master-pointer-last
//       -> release every RW handle and volume lock
//       -> fresh PhysicalDrive read-only cold verification
//
// The Rescue Capsule or legacy backup decides WHAT bytes are legitimate. The
// physical-write authorization decides WHICH device may receive them. Neither
// is allowed to substitute for the other merely because both use the word
// "verification" somewhere in their implementation.
[[nodiscard]] PhysicalBootstrapRestoreResult restore_bootstrap_to_physical(
    unsigned physical_drive_index,
    const PhysicalBootstrapRestoreOptions& options);

} // namespace ps2hdd

#endif
