#pragma once

#ifdef _WIN32

#include "ps2hdd/opl_library_sync.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace ps2hdd {

struct PhysicalLibrarySyncOptions {
    opl::PfsImportOptions pfs{};
    std::filesystem::path artifact_directory;
    bool lock_mounted_volumes{true};
    bool dismount_locked_volumes{true};
};

struct PhysicalLibrarySyncResult {
    bool ok{};
    std::string error;
    std::filesystem::path hddmbr_path;
    std::size_t locked_volume_count{};
    std::size_t verified_assets{};
    opl::LibrarySyncResult sync;
};

// Applies an already-staged library sync plan to a physical PS2 HDD using the
// same guarded sequence as physical game deploy:
//
//   PhysicalDrive (GENERIC_READ) -> clean APA + write admission
//   -> FHDB-compatible HDDMBR backup in the artifact directory
//   -> short-lived WritablePhysicalDrive lease + volume locks
//   -> apply_library_sync() (re-checks the plan's OPL partition identity)
//   -> release RW handle
//   -> fresh read-only PhysicalDrive: APA must be clean and every staged
//      asset must now appear in the OPL inventory.
//
// Planning (network + inventory) must happen beforehand on a read-only source;
// this function never downloads anything.
[[nodiscard]] PhysicalLibrarySyncResult apply_library_sync_to_physical(
    unsigned physical_drive_index,
    const opl::LibrarySyncPlan& plan,
    const PhysicalLibrarySyncOptions& options);

} // namespace ps2hdd

#endif
