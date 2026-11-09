#ifdef _WIN32

#include "ps2hdd/physical_migration.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/disk_layout_guard.hpp"
#include "ps2hdd/fhdb_rescue.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/writable_physical_drive.hpp"

#include <array>
#include <cstring>

namespace ps2hdd {

bool check_migration_target(unsigned target_physical_index, std::optional<unsigned> source_physical_index,
                            std::uint64_t required_bytes, std::uint64_t& target_bytes, std::string& error)
{
    if (source_physical_index && *source_physical_index == target_physical_index) {
        error = "The target is the source drive";
        return false;
    }
    PhysicalDrive target(target_physical_index);
    if (!target.is_open()) {
        error = "Could not open PhysicalDrive" + std::to_string(target_physical_index) + " read-only";
        return false;
    }
    target_bytes = target.size_bytes();
    if (target_bytes < required_bytes) {
        error = "PhysicalDrive" + std::to_string(target_physical_index) + " is too small for the allocated APA space";
        return false;
    }
    const auto layout = inspect_disk_layout(target);
    if (!layout.ok) {
        error = "Could not read the target's partition map: " + layout.error;
        return false;
    }
    std::array<std::byte, 1024> master{};
    if (!target.read(0, master)) {
        error = "Could not read the target's first sectors";
        return false;
    }
    const bool apa_magic = std::memcmp(master.data() + 4, "APA\0", 4) == 0;
    const bool blank = !layout.mbr_signature && !layout.gpt_header;
    if (!blank && !(apa_magic && !fhdb::is_pc_owned_master(master) && layout.allows_ps2_mutation())) {
        error = "PhysicalDrive" + std::to_string(target_physical_index) +
                " has a PC partition table. DriveForge only migrates onto blank or PS2 disks; "
                "delete its volumes / clean it in Disk Management first if it really is the right drive";
        return false;
    }
    return true;
}

PhysicalMigrationResult migrate_to_physical(BlockDevice& source, std::optional<unsigned> source_physical_index,
                                            unsigned target_physical_index, const migrate::Plan& plan,
                                            migrate::Progress progress, std::stop_token stop)
{
    PhysicalMigrationResult result;
    std::uint64_t target_bytes = 0;
    if (!check_migration_target(target_physical_index, source_physical_index, plan.required_bytes, target_bytes,
                                result.error)) {
        return result;
    }
    if (target_bytes != plan.target_bytes) {
        result.error = "Target size changed since planning";
        return result;
    }
    PhysicalDrive read_only(target_physical_index);
    const auto authorization = physical_write::authorize_exceptional_recovery(read_only);
    if (!authorization.ok) {
        result.error = "Target admission failed: " + authorization.error;
        return result;
    }
    {
        WritablePhysicalDriveOptions options;
        options.authorization = authorization.authorization;
        WritablePhysicalDrive writable(target_physical_index, options);
        if (!writable.is_open()) {
            result.error = "Could not acquire the guarded target write lease: " + writable.admission_error();
            return result;
        }
        result.locked_volume_count = writable.locked_volume_count();
        result.copy = migrate::apply(source, writable, plan, {}, std::move(progress), stop);
        if (!result.copy.ok) {
            result.error = result.copy.error;
            return result;
        }
    }
    PhysicalDrive cold(target_physical_index);
    apa::Reader reader(cold);
    if (!cold.is_open() || !reader.scan().ok()) {
        result.error = "Migration finished, but a cold read-only APA parse of the target failed";
        return result;
    }
    result.cold_verified = true;
    result.ok = true;
    return result;
}

} // namespace ps2hdd

#endif
