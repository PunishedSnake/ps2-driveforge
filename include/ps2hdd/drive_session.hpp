#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"
#include "ps2hdd/instrumented_block_device.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_export.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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

struct SessionStats {
    BlockIoStats backing_io{};
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
// Darkness calls stat/read/browse concurrently from Dokany worker threads. The
// format readers remain per-call objects while the shared backing device handles
// serialization where required; operation counters therefore use atomics.
class DriveSession {
public:
    explicit DriveSession(std::unique_ptr<BlockDevice> source);

    DriveSession(const DriveSession&) = delete;
    DriveSession& operator=(const DriveSession&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return source_ != nullptr; }
    [[nodiscard]] BlockDevice& device() noexcept { return instrumented_; }
    [[nodiscard]] const apa::ScanResult& scan_result() const noexcept { return scan_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    bool scan();
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

private:
    std::unique_ptr<BlockDevice> source_;
    InstrumentedBlockDevice instrumented_;
    apa::ScanResult scan_{};
    std::string last_error_;
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
};

} // namespace ps2hdd
