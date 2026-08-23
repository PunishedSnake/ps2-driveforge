#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_remove.hpp"
#include "ps2hdd/block_device.hpp"
#include "ps2hdd/instrumented_block_device.hpp"
#include "ps2hdd/partition_catalog.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_export.hpp"
#include "ps2hdd/read_ahead_block_device.hpp"
#include "ps2hdd/read_cache_block_device.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps2hdd {

struct SessionEntry {
    std::string name;
    pfs::BlockInfo inode{};
    std::uint16_t mode{};
    std::uint64_t size{};
    bool inode_readable{};

    [[nodiscard]] bool is_directory() const noexcept
    {
        return (mode & pfs::kModeMask) == pfs::kModeDirectory;
    }
    [[nodiscard]] bool is_regular() const noexcept
    {
        return (mode & pfs::kModeMask) == pfs::kModeRegular;
    }
};

struct BrowseResult {
    bool ok{};
    std::string error;
    std::vector<SessionEntry> entries;
};

struct StatResult {
    bool ok{};
    std::string error;
    SessionEntry entry;
};

struct ReadResult {
    bool ok{};
    std::string error;
    std::size_t bytes_read{};
};

struct SessionCacheStats {
    std::uint64_t probe_hits{};
    std::uint64_t probe_misses{};
    std::uint64_t browse_hits{};
    std::uint64_t browse_misses{};
    std::uint64_t stat_hits{};
    std::uint64_t stat_misses{};
    std::uint64_t node_hits{};
    std::uint64_t node_misses{};
    std::uint64_t evictions{};
};

struct SessionStats {
    BlockIoStats backing_io{};
    ReadAheadStats read_ahead{};
    ReadCacheStats read_cache{};
    SessionCacheStats cache{};
    std::uint64_t apa_scans{};
    std::uint64_t browse_operations{};
    std::uint64_t stat_operations{};
    std::uint64_t read_operations{};
    std::uint64_t export_operations{};
    std::uint64_t scan_time_ns{};
    std::uint64_t browse_time_ns{};
    std::uint64_t stat_time_ns{};
    std::uint64_t read_time_ns{};
    std::uint64_t export_time_ns{};
};

// One opened source plus the reusable operations frontends need. DriveSession
// deliberately keeps navigation path state out of the filesystem core: a GUI,
// CLI command, and Dokany callbacks can resolve independent paths against the
// same validated APA scan.
//
// Emilia keeps immutable metadata results across calls because every current
// source is read-only. Frieren adds narrowly-scoped post-commit snapshot updates
// for mutation coordinators without turning this normal session into a writer.
// The pipeline is parser -> 4 KiB metadata cache -> adaptive sequential read-ahead
// -> backing instrumentation -> actual device.
class DriveSession {
public:
    explicit DriveSession(std::unique_ptr<BlockDevice> source);

    DriveSession(const DriveSession&) = delete;
    DriveSession& operator=(const DriveSession&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return source_ != nullptr; }
    [[nodiscard]] BlockDevice& device() noexcept { return read_cache_; }
    [[nodiscard]] const apa::ScanResult& scan_result() const noexcept { return scan_; }
    [[nodiscard]] PartitionCatalog partition_catalog(bool include_sub_partitions = true) const
    {
        return build_partition_catalog(scan_, include_sub_partitions);
    }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    bool scan();

    // Synchronize this already-open read session after a separate mutation
    // coordinator successfully commits the exact RemovePlan. This changes only
    // the cached APA snapshot and invalidates read caches; it performs no device
    // scan and no HDL/PFS rediscovery. A failed/uncommitted plan must never be
    // passed here.
    bool apply_committed_partition_removal(const apa::RemovePlan& plan);

    [[nodiscard]] const apa::Partition* find_partition(std::string_view id) const noexcept;
    [[nodiscard]] BrowseResult browse(std::string_view partition, std::string_view path);
    [[nodiscard]] StatResult stat(std::string_view partition, std::string_view path);
    [[nodiscard]] ReadResult read_file(std::string_view partition, std::string_view path,
                                       std::uint64_t offset, std::span<std::byte> out);
    [[nodiscard]] pfs::ExportResult export_to_host(std::string_view partition,
                                                   std::string_view path,
                                                   const std::filesystem::path& destination,
                                                   pfs::ExportProgress progress = {});

    [[nodiscard]] SessionStats stats() const noexcept;
    void reset_stats() noexcept;
    void clear_caches();

private:
    static constexpr std::size_t kMaxProbeCacheEntries = 256;
    static constexpr std::size_t kMaxBrowseCacheEntries = 1024;
    static constexpr std::size_t kMaxStatCacheEntries = 8192;
    static constexpr std::size_t kMaxNodeCacheEntries = 8192;

    [[nodiscard]] pfs::ProbeResult probe_for(std::string_view partition, ApaVolume& volume);

    std::unique_ptr<BlockDevice> source_;
    InstrumentedBlockDevice instrumented_;
    ReadAheadBlockDevice read_ahead_;
    ReadCacheBlockDevice read_cache_;
    apa::ScanResult scan_{};
    std::string last_error_;

    mutable std::shared_mutex cache_mutex_;
    std::unordered_map<std::string, pfs::ProbeResult> probe_cache_;
    std::unordered_map<std::string, BrowseResult> browse_cache_;
    std::unordered_map<std::string, StatResult> stat_cache_;
    std::unordered_map<std::string, pfs::Node> node_cache_;

    std::atomic<std::uint64_t> apa_scans_{};
    std::atomic<std::uint64_t> browse_operations_{};
    std::atomic<std::uint64_t> stat_operations_{};
    std::atomic<std::uint64_t> read_operations_{};
    std::atomic<std::uint64_t> export_operations_{};
    std::atomic<std::uint64_t> scan_time_ns_{};
    std::atomic<std::uint64_t> browse_time_ns_{};
    std::atomic<std::uint64_t> stat_time_ns_{};
    std::atomic<std::uint64_t> read_time_ns_{};
    std::atomic<std::uint64_t> export_time_ns_{};
    std::atomic<std::uint64_t> probe_cache_hits_{};
    std::atomic<std::uint64_t> probe_cache_misses_{};
    std::atomic<std::uint64_t> browse_cache_hits_{};
    std::atomic<std::uint64_t> browse_cache_misses_{};
    std::atomic<std::uint64_t> stat_cache_hits_{};
    std::atomic<std::uint64_t> stat_cache_misses_{};
    std::atomic<std::uint64_t> node_cache_hits_{};
    std::atomic<std::uint64_t> node_cache_misses_{};
    std::atomic<std::uint64_t> cache_evictions_{};
};

} // namespace ps2hdd
