#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/management_model.hpp"
#include "ps2hdd/physical_discovery.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace ps2df::winui {

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
    std::string source_name;
    std::uint64_t source_size_bytes{};
    ps2hdd::StorageCharacteristics storage{};
    ps2hdd::SessionStats stats{};
    ps2hdd::ManagementProgress progress{};
    double catalog_build_ms{};
    std::vector<PartitionRowSnapshot> rows;
};

// Frontend-facing owner for one native DriveSession + ManagementModel pair.
// WinUI receives snapshots and commands only; APA/PFS/HDL parsing, caches and
// instrumentation stay in the shared native libraries.
class NativeSessionController final {
public:
    using EnrichmentProgress = std::function<void(std::size_t completed, std::size_t total)>;

    [[nodiscard]] static std::vector<ps2hdd::PhysicalDriveProbe> discover_physical_drives();

    [[nodiscard]] bool open_physical(unsigned index, std::string& error);
    [[nodiscard]] bool open_image(const std::filesystem::path& path, std::string& error);
    void close() noexcept;

    [[nodiscard]] SessionSnapshot snapshot() const;
    [[nodiscard]] BrowseSnapshot browse_pfs(std::string_view partition, std::string_view path);
    [[nodiscard]] bool export_to_host(std::string_view partition, std::string_view path,
                                      const std::filesystem::path& destination,
                                      std::string& error);

    void reset_stats() noexcept;
    void reset_enrichment();
    void enrich_hdl(EnrichmentProgress progress = {}, std::stop_token stop = {});

private:
    [[nodiscard]] bool open_source(std::unique_ptr<ps2hdd::BlockDevice> source,
                                   std::string source_name,
                                   ps2hdd::StorageCharacteristics storage,
                                   std::string& error);

    mutable std::mutex mutex_;
    std::shared_ptr<ps2hdd::DriveSession> session_;
    std::shared_ptr<ps2hdd::ManagementModel> model_;
    std::string source_name_;
    ps2hdd::StorageCharacteristics storage_{};
    double catalog_build_ms_{};
};

} // namespace ps2df::winui
