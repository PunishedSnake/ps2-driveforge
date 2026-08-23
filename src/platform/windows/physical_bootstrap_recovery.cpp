#ifdef _WIN32

#include "ps2hdd/physical_bootstrap_recovery.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/fhdb_rescue.hpp"
#include "ps2hdd/fhdb_rescue_capture.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/sha256.hpp"
#include "ps2hdd/writable_physical_drive.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ps2hdd {
namespace {

constexpr std::size_t kApaOsdStartOffset = 0x130;
constexpr std::size_t kApaOsdSizeOffset = 0x134;
constexpr std::uint64_t kSectorBytes = 512ULL;

[[nodiscard]] std::uint32_t load_u32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

[[nodiscard]] bool cold_verify_bootstrap(
    unsigned physical_drive_index,
    const fhdb::BootstrapRestorePlan& plan,
    const fhdb::BootstrapRestoreResult& applied,
    bool& master_verified,
    bool& payload_verified,
    std::string& error)
{
    PhysicalDrive cold(physical_drive_index);
    if (!cold.is_open()) {
        error = "Bootstrap mutation completed, but cold read-only PhysicalDrive reopen failed";
        return false;
    }

    std::array<std::byte, fhdb::kRescueApaHeaderBytes> master{};
    if (!cold.read(0, master) || !fhdb::is_standard_apa_master(master)) {
        error = "Bootstrap mutation completed, but the cold APA master failed validation";
        return false;
    }
    if (!fhdb::same_disk_identity(master, plan.source_master)) {
        error = "Bootstrap mutation changed APA master bytes outside the permitted checksum/OSD pointer fields";
        return false;
    }
    if (load_u32(master.data() + kApaOsdStartOffset) != applied.payload_start ||
        load_u32(master.data() + kApaOsdSizeOffset) != applied.payload_sectors) {
        error = "Cold APA master does not contain the bootstrap pointer that was committed";
        return false;
    }
    master_verified = true;

    apa::Reader reader(cold);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        error = "Bootstrap mutation completed, but cold APA chain verification failed";
        return false;
    }

    if (plan.kind == fhdb::BootstrapRestoreKind::rescue_payload && !plan.payload.empty()) {
        const auto offset = static_cast<std::uint64_t>(plan.payload_start) * kSectorBytes;
        std::vector<std::byte> payload(plan.payload.size());
        if (!cold.read(offset, payload) || payload != plan.payload) {
            error = "Bootstrap mutation completed, but cold payload byte verification failed";
            return false;
        }
    }
    payload_verified = true;
    return true;
}

[[nodiscard]] bool build_provider_install_plan(
    BlockDevice& disk,
    const bootstrap::FrozenInstallInput& input,
    fhdb::BootstrapRestorePlan& plan,
    std::string& error)
{
    // Repeat the storage-neutral frozen-plan validation at the physical trust
    // boundary. A caller cannot manufacture `ok=true`, alter crypto evidence or
    // change geometry and rely on the endpoint to trust the struct's optimism.
    if (!bootstrap::validate_frozen_payload(input, error)) {
        return false;
    }

    std::array<std::byte, fhdb::kRescueApaHeaderBytes> current{};
    if (!disk.read(0, current) || !fhdb::is_standard_apa_master(current)) {
        error = "Provider bootstrap install requires a canonical live APA master";
        return false;
    }

    const auto payload_offset = static_cast<std::uint64_t>(input.program_start_sector) * kSectorBytes;
    const auto padded_bytes = static_cast<std::uint64_t>(input.payload_sector_count) * kSectorBytes;
    if (payload_offset > disk.size_bytes() || padded_bytes > disk.size_bytes() - payload_offset) {
        error = "Provider bootstrap payload extends beyond the physical disk";
        return false;
    }

    const auto strategy = bootstrap::strategy_for(input.family);
    plan.ok = true;
    plan.kind = fhdb::BootstrapRestoreKind::rescue_payload;
    plan.source_master = current;
    plan.saved_master = current;
    plan.payload_start = input.program_start_sector;
    plan.payload_sectors = input.payload_sector_count;
    plan.family = std::string(strategy.name);
    plan.confidence = input.magicgate_signed ? "provider-signed-and-verified"
                                              : "provider-verified";
    plan.payload.assign(static_cast<std::size_t>(padded_bytes), std::byte{0});
    std::copy(input.payload.begin(), input.payload.end(), plan.payload.begin());
    return true;
}

} // namespace

PhysicalBootstrapRestoreResult restore_bootstrap_to_physical(
    unsigned physical_drive_index,
    const PhysicalBootstrapRestoreOptions& options)
{
    PhysicalBootstrapRestoreResult result;
    if (options.artifact_directory.empty()) {
        result.error = "Physical bootstrap restore requires an FHDB artifact directory";
        return result;
    }
    if (options.safety_directory.empty()) {
        result.error = "Physical bootstrap restore requires a safety-artifact directory";
        return result;
    }

    PhysicalDrive read_only(physical_drive_index);
    if (!read_only.is_open()) {
        result.error = "Could not open the selected PhysicalDrive read-only before bootstrap restore";
        return result;
    }

    const auto authorization = physical_write::authorize(read_only);
    if (!authorization.ok) {
        result.error = "Physical bootstrap restore admission failed: " + authorization.error;
        return result;
    }

    const auto plan = fhdb::plan_bootstrap_restore(read_only, options.artifact_directory);
    if (!plan.ok) {
        result.error = "FHDB bootstrap restore planning failed: " + plan.error;
        return result;
    }

    {
        WritablePhysicalDriveOptions writable_options;
        writable_options.authorization = authorization.authorization;
        writable_options.lock_mounted_volumes = options.lock_mounted_volumes;
        writable_options.dismount_locked_volumes = options.dismount_locked_volumes;

        WritablePhysicalDrive writable(physical_drive_index, writable_options);
        if (!writable.is_open()) {
            result.error = "Could not acquire guarded physical bootstrap write lease: " + writable.admission_error();
            return result;
        }
        result.locked_volume_count = writable.locked_volume_count();
        result.restore = fhdb::apply_bootstrap_restore(writable, plan, options.safety_directory);
        if (!result.restore.ok) {
            result.partial = result.restore.partial;
            result.error = "Physical bootstrap restore failed: " + result.restore.error;
            return result;
        }
    }

    if (!cold_verify_bootstrap(physical_drive_index, plan, result.restore,
                               result.cold_master_verified,
                               result.cold_payload_verified,
                               result.error)) {
        result.partial = true;
        return result;
    }
    result.ok = true;
    return result;
}

PhysicalBootstrapInstallResult install_bootstrap_to_physical(
    unsigned physical_drive_index,
    const bootstrap::FrozenInstallInput& input,
    const PhysicalBootstrapInstallOptions& options)
{
    PhysicalBootstrapInstallResult result;
    if (options.safety_directory.empty()) {
        result.error = "Physical provider bootstrap install requires a safety-artifact directory";
        return result;
    }

    std::string frozen_error;
    if (!bootstrap::validate_frozen_payload(input, frozen_error)) {
        result.error = "Frozen provider bootstrap validation failed: " + frozen_error;
        return result;
    }

    PhysicalDrive read_only(physical_drive_index);
    if (!read_only.is_open()) {
        result.error = "Could not open the selected PhysicalDrive read-only before provider bootstrap install";
        return result;
    }
    const auto authorization = physical_write::authorize(read_only);
    if (!authorization.ok) {
        result.error = "Physical provider bootstrap admission failed: " + authorization.error;
        return result;
    }
    const auto actual_fingerprint = crypto::sha256_hex(authorization.authorization.identity.digest);
    if (actual_fingerprint != input.target_fingerprint) {
        result.error = "Frozen provider bootstrap target fingerprint does not match the live physical disk";
        return result;
    }

    fhdb::BootstrapRestorePlan plan;
    if (!build_provider_install_plan(read_only, input, plan, result.error)) {
        return result;
    }

    // Installation replaces a boot-visible payload. Capture historical evidence
    // before entering RW, even when the current pointer is zero/zero. A corrupt
    // live pointer is a recovery problem and therefore blocks normal install.
    const auto rescue = fhdb::capture_rescue_image(read_only);
    if (!rescue.ok) {
        result.error = "Could not capture the mandatory pre-install Rescue Capsule: " + rescue.error;
        return result;
    }
    const auto saved_rescue = fhdb::save_hddrescue(options.safety_directory, rescue);
    if (!saved_rescue.ok) {
        result.error = "Could not persist the mandatory pre-install Rescue Capsule: " + saved_rescue.error;
        return result;
    }
    result.rescue_capsule_path = saved_rescue.path;

    {
        WritablePhysicalDriveOptions writable_options;
        writable_options.authorization = authorization.authorization;
        writable_options.lock_mounted_volumes = options.lock_mounted_volumes;
        writable_options.dismount_locked_volumes = options.dismount_locked_volumes;
        WritablePhysicalDrive writable(physical_drive_index, writable_options);
        if (!writable.is_open()) {
            result.error = "Could not acquire guarded provider bootstrap write lease: " + writable.admission_error();
            return result;
        }
        result.locked_volume_count = writable.locked_volume_count();

        // Reuse the proven payload-first / pointer-last transaction. Here the
        // plan was produced from verified provider bytes instead of a rescue
        // artifact, but the disk-safety ordering remains identical.
        result.apply = fhdb::apply_bootstrap_restore(writable, plan, options.safety_directory);
        result.hddmbr_path = result.apply.safety_backup_path;
        if (!result.apply.ok) {
            result.partial = result.apply.partial;
            result.error = "Physical provider bootstrap install failed: " + result.apply.error;
            return result;
        }
    }

    if (!cold_verify_bootstrap(physical_drive_index, plan, result.apply,
                               result.cold_master_verified,
                               result.cold_payload_verified,
                               result.error)) {
        result.partial = true;
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace ps2hdd

#endif
