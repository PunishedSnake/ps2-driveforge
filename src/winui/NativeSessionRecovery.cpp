#include "pch.h"
#include "NativeSessionController.hpp"

#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/fhdb_rescue_capture.hpp"
#include "ps2hdd/physical_bootstrap_recovery.hpp"
#include "ps2hdd/physical_drive.hpp"

namespace ps2df::winui {

RecoveryActionSnapshot NativeSessionController::capture_rescue_capsule(
    const std::filesystem::path& artifact_directory) const
{
    RecoveryActionSnapshot result;
    std::optional<unsigned> index;
    {
        std::scoped_lock lock(mutex_);
        if (source_kind_ != SourceKind::physical || !physical_index_) {
            result.error = "Rescue Capsule capture requires an open physical PS2 HDD.";
            return result;
        }
        index = physical_index_;
    }
    if (artifact_directory.empty()) {
        result.error = "Choose a host-side artifact directory before Rescue Capsule capture.";
        return result;
    }

    ps2hdd::PhysicalDrive disk(*index);
    if (!disk.is_open()) {
        result.error = "Could not reopen PhysicalDrive" + std::to_string(*index) + " read-only.";
        return result;
    }

    const auto captured = ps2hdd::fhdb::capture_rescue_image(disk);
    if (!captured.ok) {
        result.error = captured.error;
        return result;
    }
    const auto saved = ps2hdd::fhdb::save_hddrescue(artifact_directory, captured);
    if (!saved.ok) {
        result.error = saved.error;
        return result;
    }

    result.ok = true;
    result.artifact_path = saved.path;
    result.summary = "Captured canonical PS2HBRC Rescue Capsule read-only";
    return result;
}

RecoveryActionSnapshot NativeSessionController::restore_bootstrap(
    const std::filesystem::path& artifact_directory,
    const std::filesystem::path& safety_directory,
    bool physical_confirmation)
{
    RecoveryActionSnapshot result;
    std::optional<unsigned> index;
    {
        std::scoped_lock lock(mutex_);
        if (source_kind_ != SourceKind::physical || !physical_index_) {
            result.error = "Bootstrap restore requires an open physical PS2 HDD.";
            return result;
        }
        index = physical_index_;
    }
    if (!physical_confirmation) {
        result.error = "Physical bootstrap restore requires explicit target confirmation.";
        return result;
    }
    if (artifact_directory.empty()) {
        result.error = "Choose the directory containing HDDRESCUE/HDDMBR/FHDBMBR restore input.";
        return result;
    }
    if (safety_directory.empty()) {
        result.error = "Choose a safety directory for the mandatory current-master before-image.";
        return result;
    }

    ps2hdd::PhysicalBootstrapRestoreOptions options;
    options.artifact_directory = artifact_directory;
    options.safety_directory = safety_directory;
    const auto restored = ps2hdd::restore_bootstrap_to_physical(*index, options);
    result.ok = restored.ok;
    result.partial = restored.partial;
    result.error = restored.error;
    result.cold_master_verified = restored.cold_master_verified;
    result.cold_payload_verified = restored.cold_payload_verified;
    result.summary = restored.ok
        ? "Bootstrap restore committed payload-first and cold-verified read-only"
        : "Bootstrap restore did not complete";

    if (result.ok) {
        std::string reopen_error;
        if (!cold_reopen(reopen_error)) {
            result.ok = false;
            result.partial = true;
            result.error = "Bootstrap restore committed but the WinUI session could not cold-reopen it: " +
                           reopen_error;
        }
    }
    return result;
}

} // namespace ps2df::winui
