#pragma once

#ifdef _WIN32

#include "ps2hdd/image_game_deploy.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace ps2hdd {

struct PhysicalGameDeployOptions {
    GameDeployOptions game;
    std::filesystem::path artifact_directory;
    bool lock_mounted_volumes{true};
    bool dismount_locked_volumes{true};
};

struct PhysicalGameDeployResult {
    bool ok{};
    bool partial{};
    std::string error;
    std::filesystem::path hddmbr_path;
    std::filesystem::path mutation_journal_path;
    std::size_t locked_volume_count{};
    GameDeployResult deployment;
};

// Windows physical-HDD coordinator. It deliberately performs the read-only
// admission and the write-capability acquisition in different objects:
//
//   PhysicalDrive (GENERIC_READ)
//       -> game/APA/GPT preflight
//       -> physical media fingerprint
//       -> FHDB-compatible HDDMBR safety backup
//       -> WritablePhysicalDrive lease + volume locks
//       -> the same storage-neutral HDL/PFS/TAR writers used by images
//       -> release RW handle
//       -> fresh read-only PhysicalDrive + cold APA/HDL verification
//
// The artifact directory is mandatory. Physical mutation without a host-side
// safety artifact is not a convenience mode; it is just deleting the seatbelt.
[[nodiscard]] PhysicalGameDeployResult deploy_game_to_physical(
    unsigned physical_drive_index,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const PhysicalGameDeployOptions& options,
    hdl::InstallProgressCallback progress = {});

} // namespace ps2hdd

#endif
