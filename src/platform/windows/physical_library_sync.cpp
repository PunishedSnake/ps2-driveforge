#ifdef _WIN32

#include "ps2hdd/physical_library_sync.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/writable_physical_drive.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

namespace ps2hdd {
namespace {

const apa::Partition* find_pfs_main(const apa::ScanResult& scan, const std::string& id) noexcept
{
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.type == apa::kTypePfs && partition.id == id) {
            return &partition;
        }
    }
    return nullptr;
}

} // namespace

PhysicalLibrarySyncResult apply_library_sync_to_physical(unsigned physical_drive_index,
                                                         const opl::LibrarySyncPlan& plan,
                                                         const PhysicalLibrarySyncOptions& options)
{
    PhysicalLibrarySyncResult result;
    if (!plan.ok) {
        result.error = plan.error.empty() ? "library sync plan is not valid" : plan.error;
        return result;
    }
    if (plan.assets.empty()) {
        result.ok = true;
        result.sync.ok = true;
        return result;
    }
    if (options.artifact_directory.empty()) {
        result.error = "physical library sync requires a host-side recovery artifact directory";
        return result;
    }

    PhysicalDrive read_only(physical_drive_index);
    if (!read_only.is_open()) {
        result.error = "could not open target PhysicalDrive read-only for preflight";
        return result;
    }
    {
        apa::Reader reader(read_only);
        const auto scan = reader.scan();
        const auto* partition = scan.ok() ? find_pfs_main(scan, plan.opl_partition_id) : nullptr;
        if (partition == nullptr || partition->start_lba != plan.opl_partition_start_lba) {
            result.error = "physical target no longer matches the library sync plan's OPL partition";
            return result;
        }
    }

    const auto admission = physical_write::authorize(read_only);
    if (!admission.ok) {
        result.error = "physical write admission failed: " + admission.error;
        return result;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(options.artifact_directory, directory_error);
    if (directory_error) {
        result.error = "could not create physical recovery artifact directory: " +
                       directory_error.message();
        return result;
    }
    std::array<std::byte, fhdb::kRescueApaHeaderBytes> master{};
    if (!read_only.read(0, master)) {
        result.error = "could not capture current APA master before physical mutation";
        return result;
    }
    const auto master_backup = fhdb::save_hddmbr(options.artifact_directory, master);
    if (!master_backup.ok) {
        result.error = "could not persist HDDMBR backup before physical mutation: " + master_backup.error;
        return result;
    }
    result.hddmbr_path = master_backup.path;

    {
        WritablePhysicalDriveOptions write_options;
        write_options.authorization = admission.authorization;
        write_options.lock_mounted_volumes = options.lock_mounted_volumes;
        write_options.dismount_locked_volumes = options.dismount_locked_volumes;

        WritablePhysicalDrive writable(physical_drive_index, write_options);
        if (!writable.is_open()) {
            result.error = "could not acquire guarded physical write capability: " +
                           writable.admission_error();
            return result;
        }
        result.locked_volume_count = writable.locked_volume_count();
        result.sync = opl::apply_library_sync(writable, plan, options.pfs);
        if (!result.sync.ok) {
            result.error = result.sync.error;
        }
    }

    // Cold, read-only proof with the RW lease and volume locks released.
    PhysicalDrive cold(physical_drive_index);
    if (!cold.is_open()) {
        result.error += (result.error.empty() ? "" : "; ") +
                        std::string("could not reopen PhysicalDrive read-only after library sync");
        return result;
    }
    apa::Reader reader(cold);
    const auto scan = reader.scan();
    const auto* partition = scan.ok() ? find_pfs_main(scan, plan.opl_partition_id) : nullptr;
    if (partition == nullptr) {
        result.error += (result.error.empty() ? "" : "; ") +
                        std::string("cold reopen: APA is not clean or the OPL partition is missing");
        return result;
    }
    const auto inventory = opl::inventory_opl_assets(cold, *partition);
    for (const auto& asset : plan.assets) {
        if (inventory.contains(opl::asset_inventory_key(asset.target_path, asset.archive_member))) {
            ++result.verified_assets;
        }
    }
    if (result.verified_assets != plan.assets.size()) {
        result.error += (result.error.empty() ? "" : "; ") +
                        std::string("cold reopen found ") + std::to_string(result.verified_assets) +
                        " of " + std::to_string(plan.assets.size()) + " synced assets";
        return result;
    }
    result.ok = result.sync.ok;
    return result;
}

} // namespace ps2hdd

#endif
