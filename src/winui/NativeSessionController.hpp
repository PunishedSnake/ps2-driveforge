#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/management_model.hpp"
#include "ps2hdd/physical_discovery.hpp"

#include <cstddef>
#include <cstdint>
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

struct SessionSnapshot {
    bool open{};
    std::string source_name;
    ps2hdd::StorageCharacteristics storage{};
    ps2hdd::SessionStats stats{};
    ps2hdd::ManagementProgress progress{};
    double catalog_build_ms{};
    std::vector<PartitionRowSnapshot> rows;
};

// Frontend-facing owner for one native DriveSession + ManagementModel pair.
// The WinUI layer only receives immutable snapshots. Storage parsing, caching,
// instrumentation and HDL reads remain in the existing native libraries.
class NativeSessionController final {
public:
    using EnrichmentProgress = std::function<void(std::size_t completed, std::size_t total)>;

    [[nodiscard]] static std::vector<ps2hdd::PhysicalDriveProbe> discover_physical_drives();

    [[nodiscard]] bool open_physical(unsigned index, std::string& error);
    void close() noexcept;

    [[nodiscard]] SessionSnapshot snapshot() const;
    void enrich_hdl(EnrichmentProgress progress = {}, std::stop_token stop = {});

private:
    mutable std::mutex mutex_;
    std::shared_ptr<ps2hdd::DriveSession> session_;
    std::shared_ptr<ps2hdd::ManagementModel> model_;
    std::string source_name_;
    ps2hdd::StorageCharacteristics storage_{};
    double catalog_build_ms_{};
};

} // namespace ps2df::winui
