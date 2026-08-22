#include "pch.h"
#include "NativeSessionController.hpp"

#include "ps2hdd/drive_session.hpp"
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
    auto session = std::make_shared<ps2hdd::DriveSession>(std::move(physical));
    if (!session->scan()) {
        error = session->last_error();
        return false;
    }

    const auto catalog_started = Clock::now();
    auto catalog = session->partition_catalog(true);
    const auto catalog_elapsed = std::chrono::duration<double, std::milli>(Clock::now() - catalog_started).count();
    auto model = std::make_shared<ps2hdd::ManagementModel>(std::move(catalog));

    {
        std::scoped_lock lock(mutex_);
        session_ = std::move(session);
        model_ = std::move(model);
        source_name_ = source_name;
        storage_ = storage;
        catalog_build_ms_ = catalog_elapsed;
    }
    error.clear();
    return true;
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
