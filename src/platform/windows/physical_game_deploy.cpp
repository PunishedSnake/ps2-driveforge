#ifdef _WIN32

#include "ps2hdd/physical_game_deploy.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/hdl.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/writable_physical_drive.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ps2hdd {
namespace {

const apa::Partition* find_hdl_main(const apa::ScanResult& scan,
                                   std::uint32_t start_lba,
                                   std::string_view id) noexcept
{
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.type == apa::kTypeHdl &&
            partition.start_lba == start_lba && partition.id == id) {
            return &partition;
        }
    }
    return nullptr;
}

std::filesystem::path choose_journal_path(const std::filesystem::path& directory,
                                          unsigned index,
                                          std::string_view startup,
                                          std::string& error)
{
    error.clear();
    const std::string stem = "PhysicalDrive" + std::to_string(index) + "-" +
                             std::string(startup) + "-hdl-install";
    std::error_code ec;
    for (unsigned slot = 1; slot <= 999; ++slot) {
        const auto suffix = slot == 1 ? std::string{} : "-" + std::to_string(slot);
        const auto candidate = directory / (stem + suffix + ".ps2df-journal");
        const bool exists = std::filesystem::exists(candidate, ec);
        if (ec) {
            error = "could not inspect the physical mutation-journal directory: " + ec.message();
            return {};
        }
        if (!exists) {
            return candidate;
        }
    }
    error = "physical mutation-journal directory exhausted 999 unique install slots";
    return {};
}

bool verify_cold_install(unsigned index,
                         const GameDeployResult& deployment,
                         const GameDeployOptions& options,
                         std::string& error)
{
    PhysicalDrive cold(index);
    if (!cold.is_open()) {
        error = "could not reopen PhysicalDrive read-only after physical deployment";
        return false;
    }

    apa::Reader reader(cold);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        error = "physical deployment completed but cold APA reopen is not clean";
        return false;
    }

    const auto* installed = find_hdl_main(scan,
                                          deployment.game.main_start_lba,
                                          deployment.game.partition_id);
    if (installed == nullptr) {
        error = "cold physical reopen could not find the newly installed HDL main partition";
        return false;
    }

    const auto info = hdl::read_game_info(cold, *installed);
    if (!info.ok || info.game.startup != deployment.game.startup ||
        info.game.title != options.hdl.title || !info.game.allocation_table_consistent) {
        error = "cold physical reopen failed HDL metadata/allocation verification";
        return false;
    }
    return true;
}

} // namespace

PhysicalGameDeployResult deploy_game_to_physical(
    unsigned physical_drive_index,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const PhysicalGameDeployOptions& options,
    hdl::InstallProgressCallback progress)
{
    PhysicalGameDeployResult result;
    if (options.artifact_directory.empty()) {
        result.error = "physical deployment requires a host-side recovery artifact directory";
        return result;
    }

    PhysicalDrive read_only(physical_drive_index);
    if (!read_only.is_open()) {
        result.error = "could not open target PhysicalDrive read-only for preflight";
        return result;
    }

    const auto preview = preflight_game_deploy(read_only, game_iso, fetched_assets, options.game);
    if (!preview.ok) {
        result.error = "physical game preflight failed: " + preview.error;
        return result;
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
        result.error = "could not persist FHDB-compatible HDDMBR backup before physical mutation: " +
                       master_backup.error;
        return result;
    }
    result.hddmbr_path = master_backup.path;

    std::string journal_error;
    result.mutation_journal_path = choose_journal_path(options.artifact_directory,
                                                       physical_drive_index,
                                                       preview.hdl_plan.source.startup,
                                                       journal_error);
    if (result.mutation_journal_path.empty()) {
        result.error = journal_error;
        return result;
    }

    auto deploy_options = options.game;
    deploy_options.hdl.mutation_journal_path = result.mutation_journal_path;

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

        result.deployment = deploy_game(writable, game_iso, fetched_assets,
                                        deploy_options, std::move(progress));
        result.partial = result.deployment.partial;
        if (!result.deployment.ok) {
            result.error = result.deployment.error;
        }
    }

    // The RW handle and all Windows volume locks are gone before the cold reopen.
    // Verifying through the same live handle would mostly prove that caches are
    // very good at remembering what we just told them.
    std::string cold_error;
    if (result.deployment.game.ok &&
        !verify_cold_install(physical_drive_index, result.deployment,
                             deploy_options, cold_error)) {
        if (result.error.empty()) {
            result.error = cold_error;
        } else {
            result.error += "; cold reopen: " + cold_error;
        }
        result.partial = true;
        return result;
    }

    if (!result.deployment.ok) {
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd

#endif
