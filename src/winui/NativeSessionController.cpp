#include "pch.h"
#include "NativeSessionController.hpp"

#include "ps2hdd/apa_remove.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/hdl_enrichment.hpp"
#include "ps2hdd/http_client.hpp"
#include "ps2hdd/image_game_deploy.hpp"
#include "ps2hdd/opl_asset_pipeline.hpp"
#include "ps2hdd/opl_assets.hpp"
#include "ps2hdd/opl_metadata.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_game_deploy.hpp"
#include "ps2hdd/physical_partition_remove.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/ps2_iso.hpp"
#include "ps2hdd/sha256.hpp"
#include "ps2hdd/writable_file_block_device.hpp"

#include <chrono>
#include <span>
#include <utility>

namespace ps2df::winui {
namespace {
using Clock = std::chrono::steady_clock;

PartitionRowSnapshot make_row_snapshot(const ps2hdd::ManagementRow& row)
{
    PartitionRowSnapshot result;
    result.partition_id = row.partition.id;
    result.title = row.hdl_game ? row.hdl_game->title : row.partition.id;
    result.startup = row.hdl_game ? row.hdl_game->startup : std::string{};
    result.kind = row.partition.kind;
    result.enrichment = row.hdl_state;
    result.size_bytes = row.partition.size_bytes;
    result.start_lba = row.partition.start_lba;
    result.is_sub = row.partition.is_sub;
    result.error = row.enrichment_error;
    return result;
}

ps2hdd::ImageGameDeployOptions make_deploy_options(const HdlInstallRequest& request)
{
    ps2hdd::ImageGameDeployOptions options;
    options.hdl.title = request.title;
    options.hdl.hidden = request.hidden;
    options.hdl.media = request.media;
    options.hdl.compat_flags = request.compat_flags;
    options.hdl.dma = request.dma;
    options.hdl.layer_break = request.layer_break;
    return options;
}

std::uint64_t fetched_asset_bytes(std::span<const ps2hdd::opl::FetchedAsset> assets) noexcept
{
    std::uint64_t total = 0;
    for (const auto& asset : assets) total += asset.bytes;
    return total;
}

struct PlannedDeploy {
    ps2hdd::GameDeployPreview preview;
    bool use_assets{};
    std::string warning;
};

PlannedDeploy plan_with_optional_assets(ps2hdd::BlockDevice& target,
                                        ps2hdd::BlockDevice& iso,
                                        std::span<const ps2hdd::opl::FetchedAsset> assets,
                                        const ps2hdd::GameDeployOptions& options)
{
    PlannedDeploy result;
    result.preview = ps2hdd::preflight_game_deploy(target, iso, assets, options);
    result.use_assets = result.preview.ok && result.preview.has_pfs_assets;
    if (result.preview.ok || assets.empty()) return result;

    auto game_only = ps2hdd::preflight_game_deploy(
        target, iso, std::span<const ps2hdd::opl::FetchedAsset>{}, options);
    if (!game_only.ok) return result;

    result.warning = "Game plan is valid, but automatic OPL asset placement is unavailable: " +
                     result.preview.error + ". The game will be installed without disk-side OPL assets.";
    result.preview = std::move(game_only);
    result.use_assets = false;
    return result;
}

} // namespace

std::vector<ps2hdd::PhysicalDriveProbe> NativeSessionController::discover_physical_drives()
{
    return ps2hdd::discover_physical_drives();
}

bool NativeSessionController::open_source(std::unique_ptr<ps2hdd::BlockDevice> source,
                                          std::string source_name,
                                          ps2hdd::StorageCharacteristics storage,
                                          SourceKind kind,
                                          std::optional<unsigned> physical_index,
                                          std::filesystem::path image_path,
                                          std::string& error)
{
    if (!source) {
        error = "No source device was supplied.";
        return false;
    }
    auto session = std::make_shared<ps2hdd::DriveSession>(std::move(source));
    if (!session->scan()) {
        error = session->last_error();
        return false;
    }
    if (!session->scan_result().mbr_valid) {
        error = "The selected source does not contain a valid PS2 APA MBR.";
        return false;
    }

    const auto catalog_started = Clock::now();
    auto catalog = session->partition_catalog(true);
    const auto catalog_elapsed = std::chrono::duration<double, std::milli>(
        Clock::now() - catalog_started).count();
    auto model = std::make_shared<ps2hdd::ManagementModel>(std::move(catalog));
    {
        std::scoped_lock lock(mutex_);
        session_ = std::move(session);
        model_ = std::move(model);
        source_kind_ = kind;
        physical_index_ = physical_index;
        image_path_ = std::move(image_path);
        source_name_ = std::move(source_name);
        storage_ = storage;
        catalog_build_ms_ = catalog_elapsed;
    }
    error.clear();
    return true;
}

bool NativeSessionController::open_physical(unsigned index, std::string& error)
{
    auto physical = std::make_unique<ps2hdd::PhysicalDrive>(index);
    if (!physical->is_open()) {
        error = "Could not open PhysicalDrive" + std::to_string(index) +
                " read-only (Win32 error " + std::to_string(physical->open_error()) + ")";
        return false;
    }
    const auto source_name = physical->display_name();
    const auto storage = physical->storage_characteristics();
    return open_source(std::move(physical), source_name, storage,
                       SourceKind::physical, index, {}, error);
}

bool NativeSessionController::open_image(const std::filesystem::path& path, std::string& error)
{
    auto image = std::make_unique<ps2hdd::FileBlockDevice>(path);
    if (!image->is_open()) {
        error = "Could not open the selected disk image.";
        return false;
    }
    const auto source_name = image->display_name();
    const auto storage = image->storage_characteristics();
    return open_source(std::move(image), source_name, storage,
                       SourceKind::image, std::nullopt, path, error);
}

void NativeSessionController::close() noexcept
{
    std::scoped_lock lock(mutex_);
    model_.reset();
    session_.reset();
    source_kind_ = SourceKind::none;
    physical_index_.reset();
    image_path_.clear();
    source_name_.clear();
    storage_ = {};
    catalog_build_ms_ = 0.0;
}

SessionSnapshot NativeSessionController::snapshot() const
{
    std::scoped_lock lock(mutex_);
    SessionSnapshot result;
    if (!session_ || !model_) return result;
    result.open = true;
    result.source_kind = source_kind_;
    result.source_name = source_name_;
    result.source_size_bytes = session_->device().size_bytes();
    result.storage = storage_;
    result.stats = session_->stats();
    result.progress = model_->progress();
    result.catalog_build_ms = catalog_build_ms_;
    result.rows.reserve(model_->rows().size());
    for (const auto& row : model_->rows()) result.rows.emplace_back(make_row_snapshot(row));
    return result;
}

BrowseSnapshot NativeSessionController::browse_pfs(std::string_view partition, std::string_view path)
{
    std::shared_ptr<ps2hdd::DriveSession> session;
    {
        std::scoped_lock lock(mutex_);
        session = session_;
    }
    BrowseSnapshot snapshot;
    if (!session) {
        snapshot.error = "No source is open.";
        return snapshot;
    }
    const auto result = session->browse(partition, path);
    snapshot.ok = result.ok;
    snapshot.error = result.error;
    if (!result.ok) return snapshot;
    snapshot.entries.reserve(result.entries.size());
    for (const auto& entry : result.entries) {
        snapshot.entries.push_back({entry.name, entry.is_directory(), entry.is_regular(), entry.size});
    }
    return snapshot;
}

bool NativeSessionController::export_to_host(std::string_view partition, std::string_view path,
                                             const std::filesystem::path& destination,
                                             std::string& error)
{
    std::shared_ptr<ps2hdd::DriveSession> session;
    {
        std::scoped_lock lock(mutex_);
        session = session_;
    }
    if (!session) {
        error = "No source is open.";
        return false;
    }
    const auto result = session->export_to_host(partition, path, destination);
    if (!result.ok) {
        error = result.error;
        return false;
    }
    error.clear();
    return true;
}

HdlIsoPreparationSnapshot NativeSessionController::prepare_hdl_iso(
    const std::filesystem::path& iso_path,
    const std::filesystem::path& cache_root,
    bool refresh_catalog) const
{
    HdlIsoPreparationSnapshot result;
    if (iso_path.empty()) {
        result.error = "Select a PS2 ISO first.";
        return result;
    }
    if (cache_root.empty()) {
        result.error = "Automatic metadata cache path is empty.";
        return result;
    }

    ps2hdd::FileBlockDevice iso(iso_path);
    if (!iso.is_open()) {
        result.error = "Could not open the selected ISO read-only.";
        return result;
    }
    const auto inspected = ps2hdd::iso::inspect_ps2_iso(iso);
    if (!inspected.ok) {
        result.error = inspected.error;
        return result;
    }

    result.startup = inspected.game.startup;
    result.image_bytes = inspected.game.image_bytes;
    result.media = ps2hdd::opl::automatic_media_type(result.image_bytes);
    const auto game_id = ps2hdd::opl::canonical_game_id(result.startup);
    if (!game_id) {
        result.error = "Could not normalize the ISO startup ID for metadata lookup.";
        return result;
    }
    result.game_id = *game_id;

    ps2hdd::opl::GameIdentity identity;
    identity.startup = result.startup;
    identity.title = iso_path.stem().string();
    const auto plan = ps2hdd::opl::plan_assets(identity);
    if (!plan.ok) {
        result.error = plan.error;
        return result;
    }

    auto http = ps2hdd::make_platform_http_client();
    if (!http) {
        result.warnings.emplace_back("Native HTTPS is unavailable; using ISO metadata without online OPL assets.");
    } else {
        ps2hdd::opl::FetchOptions fetch_options;
        fetch_options.refresh_catalog = refresh_catalog;
        auto fetched = ps2hdd::opl::fetch_assets(plan, *http, cache_root, fetch_options);
        result.assets = std::move(fetched.assets);
        for (const auto& issue : fetched.issues) result.warnings.push_back(issue.message);
    }

    for (const auto& asset : result.assets) {
        result.asset_bytes += asset.bytes;
        if (asset.placement == ps2hdd::opl::Placement::cache_only) ++result.cache_assets;
        else if (asset.placement == ps2hdd::opl::Placement::opl_data) ++result.opl_assets;
    }

    const auto metadata = ps2hdd::opl::resolve_game_metadata(
        result.game_id, result.assets, iso_path.stem().string());
    result.title = metadata.title.empty() ? iso_path.stem().string() : metadata.title;
    result.description = metadata.description;
    result.genre = metadata.genre;
    result.release_date = metadata.release_date;
    result.developer = metadata.developer;
    result.players = metadata.players;
    result.rating = metadata.rating;
    result.title_from_database = metadata.title_from_database;
    result.cfg_found = metadata.cfg_found;

    {
        std::scoped_lock lock(mutex_);
        prepared_iso_path_ = iso_path;
        prepared_iso_assets_ = result.assets;
    }
    result.ok = true;
    return result;
}

HdlInstallPreviewSnapshot NativeSessionController::preview_hdl_install(const HdlInstallRequest& request)
{
    HdlInstallPreviewSnapshot result;
    std::shared_ptr<ps2hdd::DriveSession> session;
    SourceKind kind = SourceKind::none;
    std::string target_name;
    std::vector<ps2hdd::opl::FetchedAsset> assets = request.assets;
    {
        std::scoped_lock lock(mutex_);
        session = session_;
        kind = source_kind_;
        target_name = source_name_;
        if (assets.empty() && prepared_iso_path_ == request.iso_path) assets = prepared_iso_assets_;
    }

    result.target_kind = kind;
    result.target_name = target_name;
    result.requires_physical_confirmation = kind == SourceKind::physical;
    result.staged_asset_count = assets.size();
    result.staged_asset_bytes = fetched_asset_bytes(assets);
    if (!session || kind == SourceKind::none) {
        result.error = "Open a PS2 HDD or disk image before planning an HDL install.";
        return result;
    }
    if (request.iso_path.empty()) {
        result.error = "Select a PS2 ISO before planning the install.";
        return result;
    }
    if (request.title.empty()) {
        result.error = "HDL display title must not be empty.";
        return result;
    }

    ps2hdd::FileBlockDevice iso(request.iso_path);
    if (!iso.is_open()) {
        result.error = "Could not open the selected ISO read-only.";
        return result;
    }

    auto options = make_deploy_options(request);
    const auto planned = plan_with_optional_assets(session->device(), iso, assets, options);
    if (!planned.preview.ok) {
        result.error = planned.preview.error;
        return result;
    }

    const auto& preview = planned.preview;
    result.warning = planned.warning;
    result.assets_will_install = planned.use_assets;
    result.opl_partition_id = preview.opl_partition_id;
    result.title = preview.hdl_plan.title;
    result.startup = preview.hdl_plan.source.startup;
    result.partition_id = preview.hdl_plan.partition_id;
    result.payload_bytes = preview.hdl_plan.allocation.requested_payload_bytes;
    result.allocated_bytes = preview.hdl_plan.allocation.allocated_bytes;
    result.main_start_lba = preview.hdl_plan.allocation.main_start_lba();
    result.sub_count = preview.hdl_plan.allocation.sub_count();
    result.affected_existing_headers = preview.hdl_plan.allocation.existing_link_updates.size();
    result.ok = true;
    return result;
}

HdlMutationResultSnapshot NativeSessionController::install_hdl(
    const HdlInstallRequest& request,
    bool physical_confirmation,
    InstallProgress progress)
{
    HdlMutationResultSnapshot result;
    SourceKind kind = SourceKind::none;
    std::optional<unsigned> physical_index;
    std::filesystem::path image_path;
    std::vector<ps2hdd::opl::FetchedAsset> prepared_assets = request.assets;
    {
        std::scoped_lock lock(mutex_);
        kind = source_kind_;
        physical_index = physical_index_;
        image_path = image_path_;
        if (prepared_assets.empty() && prepared_iso_path_ == request.iso_path) prepared_assets = prepared_iso_assets_;
    }

    HdlInstallRequest frozen_request = request;
    frozen_request.assets = prepared_assets;
    const auto preview = preview_hdl_install(frozen_request);
    if (!preview.ok) {
        result.error = preview.error;
        return result;
    }
    if (kind == SourceKind::physical && !physical_confirmation) {
        result.error = "Physical HDL installation requires explicit target confirmation.";
        return result;
    }

    ps2hdd::FileBlockDevice iso(request.iso_path);
    if (!iso.is_open()) {
        result.error = "Could not reopen the selected ISO read-only.";
        return result;
    }

    const auto assets = preview.assets_will_install
        ? std::span<const ps2hdd::opl::FetchedAsset>(prepared_assets)
        : std::span<const ps2hdd::opl::FetchedAsset>{};
    result.warning = preview.warning;
    auto options = make_deploy_options(request);

    if (kind == SourceKind::image) {
        if (image_path.empty()) {
            result.error = "The current image source path is unavailable.";
            return result;
        }
        if (!request.artifact_directory.empty()) {
            options.hdl.mutation_journal_path = request.artifact_directory / "PS2DFRC1-HDL-INSTALL.BIN";
        }
        ps2hdd::WritableFileBlockDevice target(image_path);
        if (!target.is_open()) {
            result.error = "Could not reopen the current disk image for guarded write access.";
            return result;
        }
        const auto deployed = ps2hdd::deploy_game_to_image(
            target, iso, assets, options,
            [progress = std::move(progress)](const ps2hdd::hdl::ImageInstallProgress& update) {
                if (progress) progress(update.copied_bytes, update.total_bytes, update.phase);
            });
        result.ok = deployed.ok;
        result.partial = deployed.partial;
        result.error = deployed.error;
        result.partition_id = deployed.game.partition_id;
        result.startup = deployed.game.startup;
        if (!deployed.game.warning.empty()) {
            if (!result.warning.empty()) result.warning += " ";
            result.warning += deployed.game.warning;
        }
        result.affected_bytes = deployed.game.allocated_bytes;
        result.affected_headers = deployed.game.sub_count + (deployed.game.ok ? 1U : 0U);
        result.recovery_pending = deployed.game.mutation_recovery_pending;
        if (deployed.game.mutation_journal_created) result.mutation_journal_path = options.hdl.mutation_journal_path;
    } else if (kind == SourceKind::physical) {
        if (!physical_index) {
            result.error = "The current physical source index is unavailable.";
            return result;
        }
        if (request.artifact_directory.empty()) {
            result.error = "Physical HDL installation requires a host-side safety artifact directory.";
            return result;
        }
        ps2hdd::PhysicalGameDeployOptions physical_options;
        physical_options.game = options;
        physical_options.artifact_directory = request.artifact_directory;
        const auto deployed = ps2hdd::deploy_game_to_physical(
            *physical_index, iso, assets, physical_options,
            [progress = std::move(progress)](const ps2hdd::hdl::ImageInstallProgress& update) {
                if (progress) progress(update.copied_bytes, update.total_bytes, update.phase);
            });
        result.ok = deployed.ok;
        result.partial = deployed.partial;
        result.error = deployed.error;
        result.partition_id = deployed.deployment.game.partition_id;
        result.startup = deployed.deployment.game.startup;
        if (!deployed.deployment.game.warning.empty()) {
            if (!result.warning.empty()) result.warning += " ";
            result.warning += deployed.deployment.game.warning;
        }
        result.affected_bytes = deployed.deployment.game.allocated_bytes;
        result.affected_headers = deployed.deployment.game.sub_count +
                                  (deployed.deployment.game.ok ? 1U : 0U);
        result.recovery_pending = deployed.deployment.game.mutation_recovery_pending;
        result.hddmbr_path = deployed.hddmbr_path;
        result.mutation_journal_path = deployed.mutation_journal_path;
    } else {
        result.error = "No writable DriveForge source is open.";
        return result;
    }

    if (result.ok) {
        std::string reopen_error;
        if (!cold_reopen(reopen_error)) {
            result.ok = false;
            result.partial = true;
            result.error = "Mutation committed but the WinUI session could not cold-reopen it: " + reopen_error;
        }
    }
    return result;
}

HdlMutationResultSnapshot NativeSessionController::remove_hdl(
    std::uint32_t main_lba,
    const std::filesystem::path& artifact_directory,
    bool physical_confirmation)
{
    HdlMutationResultSnapshot result;
    SourceKind kind = SourceKind::none;
    std::optional<unsigned> physical_index;
    std::filesystem::path image_path;
    bool is_hdl_main = false;
    {
        std::scoped_lock lock(mutex_);
        kind = source_kind_;
        physical_index = physical_index_;
        image_path = image_path_;
        if (model_) {
            for (const auto& row : model_->rows()) {
                if (row.partition.start_lba == main_lba && !row.partition.is_sub &&
                    row.partition.kind == ps2hdd::PartitionCatalogKind::hdl) {
                    is_hdl_main = true;
                    break;
                }
            }
        }
    }
    if (!is_hdl_main) {
        result.error = "HDL Tools only removes a selected HDL main partition, never an arbitrary APA partition.";
        return result;
    }

    if (kind == SourceKind::image) {
        ps2hdd::WritableFileBlockDevice target(image_path);
        if (!target.is_open()) {
            result.error = "Could not reopen the current disk image for guarded write access.";
            return result;
        }
        const auto removed = ps2hdd::apa::remove_main_partition_from_image(target, main_lba);
        result.ok = removed.ok;
        result.error = removed.error;
        result.partition_id = removed.partition_id;
        result.affected_bytes = removed.freed_bytes;
        result.affected_headers = removed.removed_headers + removed.rewritten_headers;
    } else if (kind == SourceKind::physical) {
        if (!physical_index) {
            result.error = "The current physical source index is unavailable.";
            return result;
        }
        if (!physical_confirmation) {
            result.error = "Physical HDL removal requires explicit target confirmation.";
            return result;
        }
        if (artifact_directory.empty()) {
            result.error = "Physical HDL removal requires a host-side safety artifact directory.";
            return result;
        }
        ps2hdd::PhysicalPartitionRemoveOptions options;
        options.artifact_directory = artifact_directory;
        const auto removed = ps2hdd::remove_partition_from_physical(*physical_index, main_lba, options);
        result.ok = removed.ok;
        result.recovery_pending = removed.recovery_pending;
        result.error = removed.error;
        result.partition_id = removed.removal.partition_id;
        result.affected_bytes = removed.removal.freed_bytes;
        result.affected_headers = removed.removal.removed_headers + removed.removal.rewritten_headers;
        result.hddmbr_path = removed.hddmbr_path;
        result.mutation_journal_path = removed.mutation_journal_path;
    } else {
        result.error = "No writable DriveForge source is open.";
        return result;
    }
    if (result.ok) {
        std::string reopen_error;
        if (!cold_reopen(reopen_error)) {
            result.ok = false;
            result.partial = true;
            result.error = "Removal committed but the WinUI session could not cold-reopen it: " + reopen_error;
        }
    }
    return result;
}

PhysicalPreflightSnapshot NativeSessionController::physical_write_preflight() const
{
    PhysicalPreflightSnapshot result;
    std::optional<unsigned> index;
    {
        std::scoped_lock lock(mutex_);
        if (source_kind_ != SourceKind::physical || !physical_index_) {
            result.error = "The current source is not a physical PS2 HDD.";
            return result;
        }
        index = physical_index_;
    }
    ps2hdd::PhysicalDrive disk(*index);
    if (!disk.is_open()) {
        result.error = "Could not reopen PhysicalDrive" + std::to_string(*index) + " read-only.";
        return result;
    }
    const auto admission = ps2hdd::physical_write::authorize(disk);
    if (!admission.ok) {
        result.error = admission.error;
        return result;
    }
    result.index = *index;
    result.size_bytes = disk.size_bytes();
    result.apa_version = admission.authorization.apa_version;
    result.apa_header_count = admission.authorization.apa_header_count;
    result.fingerprint_hex = ps2hdd::crypto::sha256_hex(admission.authorization.identity.digest);
    result.ok = true;
    return result;
}

bool NativeSessionController::cold_reopen(std::string& error)
{
    SourceKind kind = SourceKind::none;
    std::optional<unsigned> index;
    std::filesystem::path image;
    {
        std::scoped_lock lock(mutex_);
        kind = source_kind_;
        index = physical_index_;
        image = image_path_;
    }
    if (kind == SourceKind::physical && index) return open_physical(*index, error);
    if (kind == SourceKind::image && !image.empty()) return open_image(image, error);
    error = "The current source cannot be cold-reopened.";
    return false;
}

void NativeSessionController::reset_stats() noexcept
{
    std::shared_ptr<ps2hdd::DriveSession> session;
    {
        std::scoped_lock lock(mutex_);
        session = session_;
    }
    if (session) session->reset_stats();
}

void NativeSessionController::reset_enrichment()
{
    std::scoped_lock lock(mutex_);
    if (!session_) return;
    model_ = std::make_shared<ps2hdd::ManagementModel>(session_->partition_catalog(true));
}

void NativeSessionController::enrich_hdl(EnrichmentProgress progress, std::stop_token stop)
{
    std::shared_ptr<ps2hdd::DriveSession> session;
    std::shared_ptr<ps2hdd::ManagementModel> model;
    {
        std::scoped_lock lock(mutex_);
        session = session_;
        model = model_;
    }
    if (!session || !model) return;
    (void)ps2hdd::enrich_hdl_catalog(
        session->device(), session->scan_result(),
        [this, model, progress = std::move(progress)](
            std::size_t completed, std::size_t total, const ps2hdd::hdl::GameResult& game) {
            {
                std::scoped_lock lock(mutex_);
                if (model_ != model) return;
                (void)model_->apply_hdl_result(game);
            }
            if (progress) progress(completed, total);
        },
        stop);
}

} // namespace ps2df::winui
