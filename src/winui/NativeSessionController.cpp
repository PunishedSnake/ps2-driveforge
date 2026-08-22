#include "pch.h"
#include "NativeSessionController.hpp"

#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/hdl_enrichment.hpp"
#include "ps2hdd/physical_drive.hpp"

#include <chrono>
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
} // namespace

std::vector<ps2hdd::PhysicalDriveProbe> NativeSessionController::discover_physical_drives()
{
    return ps2hdd::discover_physical_drives();
}

bool NativeSessionController::open_source(std::unique_ptr<ps2hdd::BlockDevice> source,
                                          std::string source_name,
                                          ps2hdd::StorageCharacteristics storage,
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

    const auto catalog_started = Clock::now();
    auto catalog = session->partition_catalog(true);
    const auto catalog_elapsed = std::chrono::duration<double, std::milli>(
        Clock::now() - catalog_started).count();
    auto model = std::make_shared<ps2hdd::ManagementModel>(std::move(catalog));

    {
        std::scoped_lock lock(mutex_);
        session_ = std::move(session);
        model_ = std::move(model);
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
    return open_source(std::move(physical), source_name, storage, error);
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
    return open_source(std::move(image), source_name, storage, error);
}

void NativeSessionController::close() noexcept
{
    std::scoped_lock lock(mutex_);
    model_.reset();
    session_.reset();
    source_name_.clear();
    storage_ = {};
    catalog_build_ms_ = 0.0;
}

SessionSnapshot NativeSessionController::snapshot() const
{
    std::scoped_lock lock(mutex_);
    SessionSnapshot result;
    if (!session_ || !model_) {
        return result;
    }

    result.open = true;
    result.source_name = source_name_;
    result.source_size_bytes = session_->device().size_bytes();
    result.storage = storage_;
    result.stats = session_->stats();
    result.progress = model_->progress();
    result.catalog_build_ms = catalog_build_ms_;
    result.rows.reserve(model_->rows().size());
    for (const auto& row : model_->rows()) {
        result.rows.emplace_back(make_row_snapshot(row));
    }
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
    if (!result.ok) {
        return snapshot;
    }

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

void NativeSessionController::reset_stats() noexcept
{
    std::shared_ptr<ps2hdd::DriveSession> session;
    {
        std::scoped_lock lock(mutex_);
        session = session_;
    }
    if (session) {
        session->reset_stats();
    }
}

void NativeSessionController::reset_enrichment()
{
    std::scoped_lock lock(mutex_);
    if (!session_) {
        return;
    }
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
    if (!session || !model) {
        return;
    }

    (void)ps2hdd::enrich_hdl_catalog(
        session->device(), session->scan_result(),
        [this, model, progress = std::move(progress)](
            std::size_t completed, std::size_t total, const ps2hdd::hdl::GameResult& game) {
            {
                std::scoped_lock lock(mutex_);
                if (model_ != model) {
                    return;
                }
                (void)model_->apply_hdl_result(game);
            }
            if (progress) {
                progress(completed, total);
            }
        },
        stop);
}

} // namespace ps2df::winui
