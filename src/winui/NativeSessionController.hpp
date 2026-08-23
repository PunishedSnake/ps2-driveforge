#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/fhdb_rescue_capture.hpp"
#include "ps2hdd/hdl_image_install.hpp"
#include "ps2hdd/management_model.hpp"
#include "ps2hdd/opl_metadata.hpp"
#include "ps2hdd/physical_bootstrap_recovery.hpp"
#include "ps2hdd/physical_discovery.hpp"
#include "ps2hdd/physical_drive.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ps2df::winui {

enum class SourceKind { none, image, physical };

struct PartitionRowSnapshot {
    std::string partition_id;
    std::string title;
    std::string startup;
    ps2hdd::PartitionCatalogKind kind{ps2hdd::PartitionCatalogKind::other};
    ps2hdd::EnrichmentState enrichment{ps2hdd::EnrichmentState::not_applicable};
    std::uint64_t size_bytes{};
    std::uint32_t start_lba{};
    bool is_sub{};
    std::string error;
};

struct BrowseEntrySnapshot {
    std::string name;
    bool is_directory{};
    bool is_regular{};
    std::uint64_t size_bytes{};
};

struct BrowseSnapshot {
    bool ok{};
    std::string error;
    std::vector<BrowseEntrySnapshot> entries;
};

struct SessionSnapshot {
    bool open{};
    SourceKind source_kind{SourceKind::none};
    std::string source_name;
    std::uint64_t source_size_bytes{};
    ps2hdd::StorageCharacteristics storage{};
    ps2hdd::SessionStats stats{};
    ps2hdd::ManagementProgress progress{};
    double catalog_build_ms{};
    std::vector<PartitionRowSnapshot> rows;
};

struct HdlIsoPreparationSnapshot {
    bool ok{};
    std::string error;
    std::string startup;
    std::string game_id;
    std::string title;
    std::string description;
    std::string genre;
    std::string release_date;
    std::string developer;
    std::string players;
    std::string rating;
    std::uint64_t image_bytes{};
    ps2hdd::hdl::MediaType media{ps2hdd::hdl::MediaType::dvd};
    std::vector<ps2hdd::opl::FetchedAsset> assets;
    std::vector<std::string> warnings;
    std::size_t cache_assets{};
    std::size_t opl_assets{};
    std::uint64_t asset_bytes{};
    bool title_from_database{};
    bool cfg_found{};
};

struct HdlInstallRequest {
    std::filesystem::path iso_path;
    std::string title;
    bool hidden{};
    ps2hdd::hdl::MediaType media{ps2hdd::hdl::MediaType::dvd};
    std::uint8_t compat_flags{};
    std::uint16_t dma{};
    std::uint32_t layer_break{};
    std::filesystem::path artifact_directory;
    std::vector<ps2hdd::opl::FetchedAsset> assets;
};

struct HdlInstallPreviewSnapshot {
    bool ok{};
    std::string error;
    std::string warning;
    SourceKind target_kind{SourceKind::none};
    std::string target_name;
    std::string title;
    std::string startup;
    std::string partition_id;
    std::uint64_t payload_bytes{};
    std::uint64_t allocated_bytes{};
    std::uint32_t main_start_lba{};
    std::size_t sub_count{};
    std::size_t affected_existing_headers{};
    std::size_t staged_asset_count{};
    std::uint64_t staged_asset_bytes{};
    bool assets_will_install{};
    std::string opl_partition_id;
    bool requires_physical_confirmation{};
};

struct HdlMutationResultSnapshot {
    bool ok{};
    bool partial{};
    bool recovery_pending{};
    std::string error;
    std::string warning;
    std::string partition_id;
    std::string startup;
    std::uint64_t affected_bytes{};
    std::size_t affected_headers{};
    std::filesystem::path hddmbr_path;
    std::filesystem::path mutation_journal_path;
};

struct PhysicalPreflightSnapshot {
    bool ok{};
    std::string error;
    unsigned index{};
    std::uint64_t size_bytes{};
    std::uint32_t apa_version{};
    std::size_t apa_header_count{};
    std::string fingerprint_hex;
};

struct RecoveryActionSnapshot {
    bool ok{};
    bool partial{};
    std::string error;
    std::string summary;
    std::filesystem::path artifact_path;
    bool cold_master_verified{};
    bool cold_payload_verified{};
};

class NativeSessionController final {
public:
    using EnrichmentProgress = std::function<void(std::size_t completed, std::size_t total)>;
    using InstallProgress = std::function<void(std::uint64_t copied, std::uint64_t total,
                                               std::string_view phase)>;

    [[nodiscard]] static std::vector<ps2hdd::PhysicalDriveProbe> discover_physical_drives();

    [[nodiscard]] bool open_physical(unsigned index, std::string& error);
    [[nodiscard]] bool open_image(const std::filesystem::path& path, std::string& error);
    void close() noexcept;

    [[nodiscard]] SessionSnapshot snapshot() const;
    [[nodiscard]] BrowseSnapshot browse_pfs(std::string_view partition, std::string_view path);
    [[nodiscard]] bool export_to_host(std::string_view partition, std::string_view path,
                                      const std::filesystem::path& destination,
                                      std::string& error);

    [[nodiscard]] HdlIsoPreparationSnapshot prepare_hdl_iso(
        const std::filesystem::path& iso_path,
        const std::filesystem::path& cache_root,
        bool refresh_catalog = false) const;

    [[nodiscard]] HdlInstallPreviewSnapshot preview_hdl_install(const HdlInstallRequest& request);
    [[nodiscard]] HdlMutationResultSnapshot install_hdl(
        const HdlInstallRequest& request, bool physical_confirmation,
        InstallProgress progress = {});
    [[nodiscard]] HdlMutationResultSnapshot remove_hdl(
        std::uint32_t main_lba, const std::filesystem::path& artifact_directory,
        bool physical_confirmation);
    [[nodiscard]] PhysicalPreflightSnapshot physical_write_preflight() const;

    [[nodiscard]] RecoveryActionSnapshot capture_rescue_capsule(
        const std::filesystem::path& artifact_directory) const
    {
        RecoveryActionSnapshot result;
        std::optional<unsigned> index;
        {
            std::scoped_lock lock(mutex_);
            if (source_kind_ != SourceKind::physical || !physical_index_) {
                result.error = "Rescue Capsule capture requires an open physical PS2 HDD.";
                return result;
            }
            index = physical_index_;
        }
        if (artifact_directory.empty()) {
            result.error = "Choose a host-side artifact directory before Rescue Capsule capture.";
            return result;
        }
        ps2hdd::PhysicalDrive disk(*index);
        if (!disk.is_open()) {
            result.error = "Could not reopen PhysicalDrive" + std::to_string(*index) + " read-only.";
            return result;
        }
        const auto captured = ps2hdd::fhdb::capture_rescue_image(disk);
        if (!captured.ok) {
            result.error = captured.error;
            return result;
        }
        const auto saved = ps2hdd::fhdb::save_hddrescue(artifact_directory, captured);
        if (!saved.ok) {
            result.error = saved.error;
            return result;
        }
        result.ok = true;
        result.artifact_path = saved.path;
        result.summary = "Captured canonical PS2HBRC Rescue Capsule read-only";
        return result;
    }

    [[nodiscard]] RecoveryActionSnapshot restore_bootstrap(
        const std::filesystem::path& artifact_directory,
        const std::filesystem::path& safety_directory,
        bool physical_confirmation)
    {
        RecoveryActionSnapshot result;
        std::optional<unsigned> index;
        {
            std::scoped_lock lock(mutex_);
            if (source_kind_ != SourceKind::physical || !physical_index_) {
                result.error = "Bootstrap restore requires an open physical PS2 HDD.";
                return result;
            }
            index = physical_index_;
        }
        if (!physical_confirmation) {
            result.error = "Physical bootstrap restore requires explicit target confirmation.";
            return result;
        }
        if (artifact_directory.empty()) {
            result.error = "Choose the directory containing HDDRESCUE/HDDMBR/FHDBMBR restore input.";
            return result;
        }
        if (safety_directory.empty()) {
            result.error = "Choose a safety directory for the mandatory current-master before-image.";
            return result;
        }
        ps2hdd::PhysicalBootstrapRestoreOptions options;
        options.artifact_directory = artifact_directory;
        options.safety_directory = safety_directory;
        const auto restored = ps2hdd::restore_bootstrap_to_physical(*index, options);
        result.ok = restored.ok;
        result.partial = restored.partial;
        result.error = restored.error;
        result.cold_master_verified = restored.cold_master_verified;
        result.cold_payload_verified = restored.cold_payload_verified;
        result.summary = restored.ok
            ? "Bootstrap restore committed payload-first and cold-verified read-only"
            : "Bootstrap restore did not complete";
        if (result.ok) {
            std::string reopen_error;
            if (!cold_reopen(reopen_error)) {
                result.ok = false;
                result.partial = true;
                result.error = "Bootstrap restore committed but the WinUI session could not cold-reopen it: " + reopen_error;
            }
        }
        return result;
    }

    void reset_stats() noexcept;
    void reset_enrichment();
    void enrich_hdl(EnrichmentProgress progress = {}, std::stop_token stop = {});

private:
    [[nodiscard]] bool open_source(std::unique_ptr<ps2hdd::BlockDevice> source,
                                   std::string source_name,
                                   ps2hdd::StorageCharacteristics storage,
                                   SourceKind kind,
                                   std::optional<unsigned> physical_index,
                                   std::filesystem::path image_path,
                                   std::string& error);
    [[nodiscard]] bool cold_reopen(std::string& error);

    mutable std::mutex mutex_;
    std::shared_ptr<ps2hdd::DriveSession> session_;
    std::shared_ptr<ps2hdd::ManagementModel> model_;
    SourceKind source_kind_{SourceKind::none};
    std::optional<unsigned> physical_index_;
    std::filesystem::path image_path_;
    std::string source_name_;
    ps2hdd::StorageCharacteristics storage_{};
    double catalog_build_ms_{};

    // Host-staged assets are bound to the exact selected ISO path. They are
    // copied into preview/install snapshots, never used as an implicit writable
    // capability and replaced atomically after each successful preparation.
    mutable std::filesystem::path prepared_iso_path_;
    mutable std::vector<ps2hdd::opl::FetchedAsset> prepared_iso_assets_;
};

} // namespace ps2df::winui
