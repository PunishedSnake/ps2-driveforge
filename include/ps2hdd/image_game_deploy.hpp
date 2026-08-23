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

// Read-only admission for the complete image workflow. This freezes staged OPL
// asset bytes, resolves one exact existing PFS destination, validates the ISO
// and HDL allocation plan, and rejects PC GPT ownership or dirty APA state.
[[nodiscard]] ImageGameDeployPreview preflight_image_game_deploy(
    BlockDevice& disk,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const ImageGameDeployOptions& options = {});

// Image-only orchestration. The HDL partition is published first. If PFS assets
// were requested, the disk is rescanned and the exact PFS main partition chosen
// during preflight must still exist at the same LBA before the frozen asset
// snapshot is applied. Failure after HDL publication is reported as partial.
[[nodiscard]] ImageGameDeployResult deploy_game_to_image(
    WritableBlockDevice& disk,
    BlockDevice& game_iso,
    std::span<const opl::FetchedAsset> fetched_assets,
    const ImageGameDeployOptions& options = {},
    hdl::ImageInstallProgressCallback progress = {});

} // namespace ps2hdd
