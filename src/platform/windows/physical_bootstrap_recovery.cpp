#ifdef _WIN32

#include "ps2hdd/physical_bootstrap_recovery.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/fhdb_rescue.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/writable_physical_drive.hpp"

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

    // Bootstrap restore operates on a canonical live master and therefore uses
    // normal mutation admission. Damaged-master repair is a different trust
    // domain and must enter through authorize_exceptional_recovery plus an APA
    // forensic/repair plan instead of smuggling recovery bytes through here.
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
            result.error = "Could not acquire guarded physical bootstrap write lease: " +
                           writable.admission_error();
            return result;
        }
        result.locked_volume_count = writable.locked_volume_count();

        // apply_bootstrap_restore() owns the format-critical ordering and the
        // mandatory HDDMBR before-image. Do not duplicate that sequence here;
        // otherwise image and physical recovery would quietly become two
        // different recovery products sharing only a name.
        result.restore = fhdb::apply_bootstrap_restore(
            writable, plan, options.safety_directory);
        if (!result.restore.ok) {
            result.partial = result.restore.partial;
            result.error = "Physical bootstrap restore failed: " + result.restore.error;
            return result;
        }
    }

    // The RW handle and every locked/dismounted Windows volume are gone before
    // this point. Reopen through the ordinary read-only class so successful
    // verification cannot accidentally rely on state cached inside the writer.
    PhysicalDrive cold(physical_drive_index);
    if (!cold.is_open()) {
        result.partial = true;
        result.error = "Bootstrap restore completed, but cold read-only PhysicalDrive reopen failed";
        return result;
    }

    std::array<std::byte, fhdb::kRescueApaHeaderBytes> master{};
    if (!cold.read(0, master) || !fhdb::is_standard_apa_master(master)) {
        result.partial = true;
        result.error = "Bootstrap restore completed, but the cold APA master failed validation";
        return result;
    }
    if (!fhdb::same_disk_identity(master, plan.source_master)) {
        result.partial = true;
        result.error = "Bootstrap restore changed APA master bytes outside the permitted checksum/OSD pointer fields";
        return result;
    }
    if (load_u32(master.data() + kApaOsdStartOffset) != result.restore.payload_start ||
        load_u32(master.data() + kApaOsdSizeOffset) != result.restore.payload_sectors) {
        result.partial = true;
        result.error = "Cold APA master does not contain the bootstrap pointer that was committed";
        return result;
    }
    result.cold_master_verified = true;

    apa::Reader reader(cold);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        result.partial = true;
        result.error = "Bootstrap restore completed, but cold APA chain verification failed";
        return result;
    }

    if (plan.kind == fhdb::BootstrapRestoreKind::rescue_payload && !plan.payload.empty()) {
        const auto offset = static_cast<std::uint64_t>(plan.payload_start) * kSectorBytes;
        std::vector<std::byte> payload(plan.payload.size());
        if (!cold.read(offset, payload) || payload != plan.payload) {
            result.partial = true;
            result.error = "Bootstrap restore completed, but cold payload byte verification failed";
            return result;
        }
    }
    // For a legacy pointer-only restore there is intentionally no payload write
    // to verify. Marking this true means every payload obligation of this plan
    // has been satisfied, not that DriveForge invented bytes absent from HDDMBR.
    result.cold_payload_verified = true;
    result.ok = true;
    return result;
}

} // namespace ps2hdd

#endif
