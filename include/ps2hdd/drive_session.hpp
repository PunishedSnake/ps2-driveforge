#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"
#include "ps2hdd/instrumented_block_device.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_export.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
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

struct SessionStats {
    BlockIoStats backing_io{};
    std::uint64_t apa_scans{};
    std::uint64_t browse_operations{};
    std::uint64_t export_operations{};
};

// One opened source plus the reusable operations frontends need. DriveSession
// deliberately keeps navigation path state out of the filesystem core: a GUI,
// CLI command, and future Dokany callback can resolve independent paths against
// the same validated APA scan.
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
    std::uint64_t apa_scans_{};
    std::uint64_t browse_operations_{};
    std::uint64_t export_operations_{};
};

} // namespace ps2hdd
