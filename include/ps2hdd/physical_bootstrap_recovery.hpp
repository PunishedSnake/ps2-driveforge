#pragma once

#ifdef _WIN32

#include "ps2hdd/bootstrap_provider.hpp"
#include "ps2hdd/fhdb_restore.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace ps2hdd {

struct PhysicalBootstrapRestoreOptions {
    std::filesystem::path artifact_directory;
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

struct PhysicalBootstrapInstallOptions {
    // Every install persists a current Rescue Capsule plus HDDMBR before-image
    // here before the guarded writer can publish a new bootstrap pointer.
    std::filesystem::path safety_directory;
    bool lock_mounted_volumes{true};
    bool dismount_locked_volumes{true};
};

struct PhysicalBootstrapInstallResult {
    bool ok{};
    bool partial{};
    std::string error;
    std::size_t locked_volume_count{};
    std::filesystem::path rescue_capsule_path;
    std::filesystem::path hddmbr_path;
    bool cold_master_verified{};
    bool cold_payload_verified{};
    fhdb::BootstrapRestoreResult apply;
};

[[nodiscard]] PhysicalBootstrapRestoreResult restore_bootstrap_to_physical(
    unsigned physical_drive_index,
    const PhysicalBootstrapRestoreOptions& options);

// Consume only a fully staged/frozen provider input. Networking, archive
// discovery and MagicGate transformation are deliberately absent here. The
// endpoint rechecks source hash, KELF structure, frozen media fingerprint and
// live APA admission before it creates any writable handle.
[[nodiscard]] PhysicalBootstrapInstallResult install_bootstrap_to_physical(
    unsigned physical_drive_index,
    const bootstrap::FrozenInstallInput& input,
    const PhysicalBootstrapInstallOptions& options);

} // namespace ps2hdd

#endif
