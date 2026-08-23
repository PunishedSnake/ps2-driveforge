#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/hdl_image_install.hpp"
#include "ps2hdd/hdl_install_plan.hpp"
#include "ps2hdd/opl_asset_pipeline.hpp"
#include "ps2hdd/opl_pfs_import.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace ps2hdd {

struct ImageGameDeployOptions {
    hdl::ImageInstallOptions hdl{};
    opl::PfsImportOptions pfs{};
    std::optional<std::string> opl_partition_id;
};

struct ImageGameDeployPreview {
    bool ok{};
    std::string error;
    hdl::InstallPlan hdl_plan;
    opl::PreparedPfsImport assets;
    bool has_pfs_assets{};
    std::string opl_partition_id;
    std::uint32_t opl_partition_start_lba{};
};

struct ImageGameDeployResult {
    bool ok{};
    bool partial{};
    std::string error;
    std::string opl_partition_id;
    std::uint32_t opl_partition_start_lba{};
    hdl::ImageInstallResult game;
    opl::PfsImportResult assets;
};

// Historical image-spelled entry points remain for current callers and tests.
// Their implementation already accepts WritableBlockDevice and is shared with
// physical media once the caller has passed the stronger physical admission.
[[nodiscard]] ImageGameDeployPreview preflight_image_game_deploy(
    BlockDevice& disk,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const ImageGameDeployOptions& options = {});

[[nodiscard]] ImageGameDeployResult deploy_game_to_image(
    WritableBlockDevice& disk,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const ImageGameDeployOptions& options = {},
    hdl::ImageInstallProgressCallback progress = {});

using GameDeployOptions = ImageGameDeployOptions;
using GameDeployPreview = ImageGameDeployPreview;
using GameDeployResult = ImageGameDeployResult;

[[nodiscard]] inline GameDeployPreview preflight_game_deploy(
    BlockDevice& disk,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const GameDeployOptions& options = {})
{
    return preflight_image_game_deploy(disk, game_iso, fetched_assets, options);
}

// Storage-neutral spelling for new callers. The destination may be an image or
// an already-authorized physical device. This layer deliberately does not open
// raw disks itself; capability acquisition is a separate safety domain.
[[nodiscard]] inline GameDeployResult deploy_game(
    WritableBlockDevice& disk,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const GameDeployOptions& options = {},
    hdl::InstallProgressCallback progress = {})
{
    return deploy_game_to_image(disk, game_iso, fetched_assets, options, std::move(progress));
}

} // namespace ps2hdd
