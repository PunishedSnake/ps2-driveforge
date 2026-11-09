#ifdef _WIN32

#include "ps2hdd/physical_partition_remove.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/mutation_journal.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/writable_physical_drive.hpp"
#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace ps2hdd {
namespace {

std::filesystem::path choose_remove_journal_path(
    const std::filesystem::path& directory,
    unsigned index,
    std::uint32_t main_lba,
    std::string& error)
{
    error.clear();
    const std::string stem = "PhysicalDrive" + std::to_string(index) +
                             "-LBA" + std::to_string(main_lba) + "-apa-remove";
    std::error_code ec;
    for (unsigned slot = 1; slot <= 999; ++slot) {
        const auto suffix = slot == 1 ? std::string{} : "-" + std::to_string(slot);
        const auto candidate = directory / (stem + suffix + ".ps2df-journal");
        const bool exists = std::filesystem::exists(candidate, ec);
        if (ec) {
            error = "could not inspect physical removal journal directory: " + ec.message();
            return {};
        }
        if (!exists) {
            return candidate;
        }
    }
    error = "physical removal journal directory exhausted 999 unique slots";
    return {};
}

bool cold_verify_removed(unsigned index,
                         const apa::RemovePlan& plan,
                         std::string& error)
{
    PhysicalDrive cold(index);
    if (!cold.is_open()) {
        error = "could not reopen PhysicalDrive read-only after removal";
        return false;
    }

    apa::Reader reader(cold);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        error = "physical removal completed but cold APA reopen is not clean";
        return false;
    }
    for (const auto lba : plan.removed_lbas) {
        if (std::any_of(scan.partitions.begin(), scan.partitions.end(),
                        [lba](const apa::Partition& partition) {
                            return partition.start_lba == lba;
                        })) {
            error = "cold physical reopen still reaches a removed APA header";
            return false;
        }
    }
    return true;
}

} // namespace

PhysicalPartitionRemoveResult remove_partition_from_physical(
    unsigned physical_drive_index,
    std::uint32_t main_lba,
    const PhysicalPartitionRemoveOptions& options)
{
    PhysicalPartitionRemoveResult result;
    if (options.artifact_directory.empty()) {
        result.error = "physical removal requires a host-side recovery artifact directory";
        return result;
    }

    PhysicalDrive read_only(physical_drive_index);
    if (!read_only.is_open()) {
        result.error = "could not open target PhysicalDrive read-only for removal preflight";
        return result;
    }

    const auto admission = physical_write::authorize(read_only);
    if (!admission.ok) {
        result.error = "physical write admission failed: " + admission.error;
        return result;
    }

    apa::Reader reader(read_only);
    const auto before = reader.scan();
    const auto plan = apa::plan_remove_main_partition(before, main_lba);
    if (!plan.ok) {
        result.error = "physical APA removal preflight failed: " + plan.error;
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
        result.error = "could not capture current APA master before physical removal";
        return result;
    }
    const auto master_backup = fhdb::save_hddmbr(options.artifact_directory, master);
    if (!master_backup.ok) {
        result.error = "could not persist FHDB-compatible HDDMBR backup before removal: " +
                       master_backup.error;
        return result;
    }
    result.hddmbr_path = master_backup.path;

    std::string journal_error;
    result.mutation_journal_path = choose_remove_journal_path(
        options.artifact_directory, physical_drive_index, main_lba, journal_error);
    if (result.mutation_journal_path.empty()) {
        result.error = journal_error;
        return result;
    }

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

        WriteTransaction transaction(writable);
        if (const auto error = apa::stage_remove_main_partition(writable, plan, transaction);
            !error.empty()) {
            result.error = error;
            return result;
        }

        const auto journal = create_mutation_journal(
            result.mutation_journal_path, writable, transaction.staged_writes());
        if (!journal.ok) {
            result.error = "could not persist physical removal Mutation Journal: " + journal.error;
            return result;
        }

        const auto committed = transaction.commit([&]() -> std::string {
            apa::Reader verify_reader(writable);
            const auto after = verify_reader.scan();
            if (!after.ok()) {
                return "APA rescan failed after physical removal";
            }
            for (const auto lba : plan.removed_lbas) {
                if (std::any_of(after.partitions.begin(), after.partitions.end(),
                                [lba](const apa::Partition& partition) {
                                    return partition.start_lba == lba;
                                })) {
                    return "removed APA header is still reachable after physical unlink";
                }
            }
            return {};
        });

        if (!committed.ok) {
            const auto restored = restore_prepared_mutation_journal(
                result.mutation_journal_path, writable);
            result.recovery_pending = !restored.ok;
            result.error = "physical APA removal transaction failed: " + committed.error;
            if (!restored.ok) {
                result.error += "; Mutation Journal recovery remains pending: " + restored.error;
            }
            return result;
        }

        const auto marked = mark_mutation_journal_committed(result.mutation_journal_path);
        if (!marked.ok) {
            result.recovery_pending = true;
            result.error = "physical removal committed, but Mutation Journal could not be marked COMMITTED: " +
                           marked.error;
            return result;
        }

        result.removal.ok = true;
        result.removal.partition_id = plan.partition_id;
        result.removal.removed_headers = plan.removed_lbas.size();
        result.removal.rewritten_headers = plan.link_rewrites.size();
        result.removal.freed_bytes = plan.freed_bytes;
    }

    std::string cold_error;
    if (!cold_verify_removed(physical_drive_index, plan, cold_error)) {
        result.error = cold_error;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd

#endif
