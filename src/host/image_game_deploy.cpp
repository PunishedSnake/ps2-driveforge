#include "ps2hdd/image_game_deploy.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/disk_layout_guard.hpp"
#include "ps2hdd/hdl.hpp"
#include "ps2hdd/opl_partition.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ps2hdd {
namespace {

const apa::Partition* find_main_at(const apa::ScanResult& scan,
                                   std::uint32_t start_lba,
                                   std::string_view id,
                                   std::uint16_t type) noexcept
{
    const apa::Partition* found = nullptr;
    for (const auto& partition : scan.partitions) {
        if (partition.is_sub() || partition.start_lba != start_lba ||
            partition.id != id || partition.type != type) {
            continue;
        }
        if (found != nullptr) {
            return nullptr;
        }
        found = &partition;
    }
    return found;
}

bool partition_id_exists(const apa::ScanResult& scan, std::string_view id) noexcept
{
    return std::any_of(scan.partitions.begin(), scan.partitions.end(), [&](const auto& partition) {
        return !partition.is_sub() && partition.id == id;
    });
}

} // namespace

ImageGameDeployPreview preflight_image_game_deploy(
    BlockDevice& disk,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const ImageGameDeployOptions& options)
{
    ImageGameDeployPreview preview;
    if (&disk == &game_iso) {
        preview.error = "Game ISO and target disk must be different block devices";
        return preview;
    }
    if (options.hdl.copy_buffer_bytes < 2048 || options.hdl.copy_buffer_bytes % 2048 != 0) {
        preview.error = "HDL copy buffer size must be a non-zero multiple of 2048 bytes";
        return preview;
    }
    if (options.hdl.media != hdl::MediaType::cd && options.hdl.media != hdl::MediaType::dvd) {
        preview.error = "Image deployment requires explicit CD or DVD media type";
        return preview;
    }

    const auto layout = inspect_disk_layout(disk);
    if (!layout.ok) {
        preview.error = "Could not validate target partition-map ownership: " + layout.error;
        return preview;
    }
    if (!layout.allows_ps2_mutation()) {
        preview.error = "Refusing image deployment because target contains " +
                        std::string(legacy_partition_map_name(layout.kind)) + " evidence";
        return preview;
    }

    apa::Reader reader(disk);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        preview.error = "Refusing image deployment because target APA scan is not clean";
        return preview;
    }

    preview.hdl_plan = hdl::plan_install(scan, disk.size_bytes(), game_iso,
                                         options.hdl.title, options.hdl.hidden);
    if (!preview.hdl_plan.ok) {
        preview.error = "HDL install preflight failed: " + preview.hdl_plan.error;
        return preview;
    }
    if (partition_id_exists(scan, preview.hdl_plan.partition_id)) {
        preview.error = "HDL APA partition ID already exists on target: " +
                        preview.hdl_plan.partition_id;
        return preview;
    }

    preview.assets = opl::prepare_pfs_import(fetched_assets, options.pfs);
    if (!preview.assets.ok) {
        preview.error = preview.assets.error.empty()
                            ? "OPL asset preflight failed"
                            : preview.assets.error;
        return preview;
    }
    preview.has_pfs_assets = !preview.assets.files.empty() || !preview.assets.archives.empty();
    if (preview.has_pfs_assets) {
        std::optional<std::string_view> configured;
        if (options.opl_partition_id && !options.opl_partition_id->empty()) {
            configured = *options.opl_partition_id;
        }
        const auto resolution = opl::resolve_data_partition(disk, scan, configured);
        if (!resolution.ok || resolution.partition_index >= scan.partitions.size()) {
            preview.error = resolution.error.empty()
                                ? "Could not resolve one unambiguous OPL PFS data partition"
                                : resolution.error;
            return preview;
        }
        const auto& partition = scan.partitions[resolution.partition_index];
        if (partition.is_sub() || partition.type != apa::kTypePfs) {
            preview.error = "Resolved OPL data target is not a main PFS partition";
            return preview;
        }
        preview.opl_partition_id = partition.id;
        preview.opl_partition_start_lba = partition.start_lba;
    }

    preview.ok = true;
    return preview;
}

ImageGameDeployResult deploy_game_to_image(
    WritableBlockDevice& disk,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const ImageGameDeployOptions& options,
    hdl::ImageInstallProgressCallback progress)
{
    ImageGameDeployResult result;
    auto preview = preflight_image_game_deploy(disk, game_iso, fetched_assets, options);
    if (!preview.ok) {
        result.error = preview.error;
        return result;
    }

    result.opl_partition_id = preview.opl_partition_id;
    result.opl_partition_start_lba = preview.opl_partition_start_lba;
    const bool has_pfs_assets = preview.has_pfs_assets;
    auto frozen_assets = std::move(preview.assets);

    result.game = hdl::install_to_image(disk, game_iso, options.hdl, std::move(progress));
    if (!result.game.ok) {
        result.error = "HDL image deployment failed: " + result.game.error;
        result.partial = result.game.orphan_payload_bytes_on_failure != 0;
        return result;
    }

    if (!has_pfs_assets) {
        result.ok = true;
        return result;
    }

    // Structural APA publication can rewrite neighbour links. Never keep using
    // the old Partition object even though the PFS payload extents are unchanged.
    apa::Reader after_hdl_reader(disk);
    const auto after_hdl = after_hdl_reader.scan();
    if (!after_hdl.ok()) {
        result.error = "HDL game is installed but APA did not rescan cleanly before OPL deployment";
        result.partial = true;
        return result;
    }
    const auto* pfs_partition = find_main_at(after_hdl,
                                              result.opl_partition_start_lba,
                                              result.opl_partition_id,
                                              apa::kTypePfs);
    if (pfs_partition == nullptr) {
        result.error = "HDL game is installed but the preflight-selected OPL PFS partition moved or disappeared";
        result.partial = true;
        return result;
    }

    result.assets = opl::import_prepared_assets(disk, *pfs_partition,
                                                 std::move(frozen_assets), options.pfs);
    if (!result.assets.ok) {
        result.error = "HDL game is installed but OPL asset deployment failed: " + result.assets.error;
        result.partial = true;
        return result;
    }

    // Final cold structural verification. Individual PFS writes already verify
    // their bytes; this additionally proves the APA chain and HDL metadata still
    // parse after all filesystem-side mutations have completed.
    apa::Reader final_reader(disk);
    const auto final_scan = final_reader.scan();
    if (!final_scan.ok()) {
        result.error = "Game and assets were written but final APA verification failed";
        result.partial = true;
        return result;
    }
    const auto* installed = find_main_at(final_scan, result.game.main_start_lba,
                                         result.game.partition_id, apa::kTypeHdl);
    const auto* final_pfs = find_main_at(final_scan, result.opl_partition_start_lba,
                                         result.opl_partition_id, apa::kTypePfs);
    if (installed == nullptr || final_pfs == nullptr) {
        result.error = "Final APA verification could not find the installed game or selected OPL PFS partition";
        result.partial = true;
        return result;
    }
    const auto game = hdl::read_game_info(disk, *installed);
    if (!game.ok || game.game.startup != result.game.startup ||
        game.game.title != options.hdl.title || !game.game.allocation_table_consistent) {
        result.error = "Final HDL metadata verification failed after OPL asset deployment";
        result.partial = true;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd
