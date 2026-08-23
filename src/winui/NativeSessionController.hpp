#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/hdl_image_install.hpp"
#include "ps2hdd/management_model.hpp"
#include "ps2hdd/physical_discovery.hpp"

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

enum class SourceKind {
    none,
    image,
    physical,
};

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

struct HdlInstallRequest {
    std::filesystem::path iso_path;
    std::string title;
    bool hidden{};
    ps2hdd::hdl::MediaType media{ps2hdd::hdl::MediaType::dvd};
    std::uint8_t compat_flags{};
    std::uint16_t dma{};
    std::uint32_t layer_break{};
    std::filesystem::path artifact_directory;
};

struct HdlInstallPreviewSnapshot {
    bool ok{};
    std::string error;
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

// Frontend-facing owner for one native DriveSession + ManagementModel pair.
// WinUI receives snapshots and commands only. Read paths stay on DriveSession;
// Frieren mutation methods reopen the selected image/PhysicalDrive through the
// guarded writer coordinators and then cold-reopen the ordinary read session.
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

    [[nodiscard]] HdlInstallPreviewSnapshot preview_hdl_install(const HdlInstallRequest& request);
    [[nodiscard]] HdlMutationResultSnapshot install_hdl(
        const HdlInstallRequest& request,
        bool physical_confirmation,
        InstallProgress progress = {});
    [[nodiscard]] HdlMutationResultSnapshot remove_hdl(
        std::uint32_t main_lba,
        const std::filesystem::path& artifact_directory,
        bool physical_confirmation);
    [[nodiscard]] PhysicalPreflightSnapshot physical_write_preflight() const;

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
};

} // namespace ps2df::winui
