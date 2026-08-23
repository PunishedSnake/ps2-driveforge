#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/writable_apa_volume.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps2hdd::pfs {

struct FileWriteOptions {
    std::uint16_t mode{static_cast<std::uint16_t>(kModeRegular | 0x01B6U)};
    std::uint16_t uid{0xFFFFU};
    std::uint16_t gid{0xFFFFU};
    DateTime timestamp{};
    std::size_t io_batch_bytes{256U * 1024U};
};

struct DirectoryWriteOptions {
    std::uint16_t mode{static_cast<std::uint16_t>(kModeDirectory | 0x01FFU)};
    std::uint16_t uid{0xFFFFU};
    std::uint16_t gid{0xFFFFU};
    DateTime timestamp{};
};

enum class FileWriteDisposition {
    created,
    replaced,
};

struct FileWriteResult {
    bool ok{};
    FileWriteDisposition disposition{FileWriteDisposition::created};
    std::string error;
    std::string warning;
    std::string path;
    std::uint64_t bytes{};
    std::size_t metadata_transactions{};
    std::size_t payload_write_calls{};
    std::size_t bitmap_chunks_touched{};
};

struct DirectoryEnsureResult {
    bool ok{};
    std::string error;
    std::string warning;
    std::string path;
    std::size_t created_components{};
    std::size_t metadata_transactions{};
    std::size_t bitmap_chunks_touched{};
};

struct FileRemoveResult {
    bool ok{};
    std::string error;
    std::string warning;
    std::string path;
    std::uint64_t bytes_freed{};
    std::size_t metadata_transactions{};
    std::size_t bitmap_chunks_touched{};
};

struct DirectoryRemoveResult {
    bool ok{};
    std::string error;
    std::string warning;
    std::string path;
    std::uint64_t bytes_freed{};
    std::size_t metadata_transactions{};
    std::size_t bitmap_chunks_touched{};
};

struct TreeRemoveResult {
    bool ok{};
    bool partial{};
    std::string error;
    std::string warning;
    std::string path;
    std::size_t files_removed{};
    std::size_t directories_removed{};
    std::uint64_t bytes_freed{};
    std::size_t metadata_transactions{};
    std::size_t bitmap_chunks_touched{};
};

// Image-only PFS mutation session. It deliberately owns a writable APA extent
// capability and never accepts PhysicalDrive. Regular files use copy-on-write
// replacement; directory creation allocates and publishes complete `.` / `..`
// directory nodes. One validated PFS probe and lazy bitmap cache are retained
// across the whole session so batch imports do not rebuild filesystem state for
// every asset.
//
// The original bounded write_file()/ensure_directory() entry points are kept as
// regression-friendly primitives. write_file_full adds directory growth and
// fragmented direct extents. write_file_complete extends that path with chained
// SEGI metadata while preserving the same cold-reader verification boundary.
class ImageWriter final {
public:
    using BitmapKey = std::pair<std::size_t, std::uint32_t>;
    using BitmapBytes = std::array<std::byte, kMetadataSize>;

    struct ZoneRun {
        std::size_t subpart{};
        std::uint32_t first{};
        std::uint32_t count{};
    };

    ImageWriter(WritableBlockDevice& device, const apa::Partition& partition);

    [[nodiscard]] bool valid() const noexcept { return probe_.valid; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const ProbeResult& probe_result() const noexcept { return probe_; }

    [[nodiscard]] DirectoryEnsureResult ensure_directory(
        std::string_view path,
        const DirectoryWriteOptions& options = {});

    [[nodiscard]] DirectoryEnsureResult ensure_directory_full(
        std::string_view path,
        const DirectoryWriteOptions& options = {});

    [[nodiscard]] FileWriteResult write_file(std::string_view path,
                                             std::span<const std::byte> bytes,
                                             const FileWriteOptions& options = {});

    [[nodiscard]] FileWriteResult write_file_full(std::string_view path,
                                                  std::span<const std::byte> bytes,
                                                  const FileWriteOptions& options = {});

    [[nodiscard]] FileWriteResult write_file_complete(
        std::string_view path,
        std::span<const std::byte> bytes,
        const FileWriteOptions& options = {});

    [[nodiscard]] FileRemoveResult remove_file(std::string_view path);
    [[nodiscard]] FileRemoveResult remove_file_full(std::string_view path);

    // Empty-directory unlink uses the same namespace-first publication rule as
    // file deletion. Recursive tree deletion is intentionally explicit and
    // reports partial=true if a later child fails after earlier removals commit.
    [[nodiscard]] DirectoryRemoveResult remove_empty_directory(std::string_view path);
    [[nodiscard]] TreeRemoveResult remove_tree(std::string_view path);

private:
    struct DirectorySlot {
        bool ok{};
        std::string error;
        std::size_t subpart{};
        std::uint32_t sector{};
        std::array<std::byte, apa::kSectorSize> sector_bytes{};
    };

    struct DentryLocation {
        bool ok{};
        std::string error;
        std::size_t subpart{};
        std::uint32_t sector{};
        std::size_t offset_in_sector{};
        std::uint16_t allocated{};
        std::array<std::byte, apa::kSectorSize> sector_bytes{};
    };

    [[nodiscard]] unsigned inode_scale() const noexcept;
    [[nodiscard]] std::uint32_t sectors_per_zone() const noexcept;
    [[nodiscard]] std::uint64_t zones_in_subpart(std::size_t subpart) const noexcept;
    [[nodiscard]] std::uint32_t bitmap_sector(std::size_t subpart,
                                              std::uint32_t chunk) const noexcept;
    [[nodiscard]] BitmapBytes* bitmap(std::size_t subpart, std::uint32_t chunk);
    [[nodiscard]] bool zone_used(std::size_t subpart, std::uint32_t zone);
    [[nodiscard]] bool set_zone(std::size_t subpart, std::uint32_t zone,
                                bool used, std::vector<BitmapKey>& dirty,
                                std::string& error);
    [[nodiscard]] ZoneRun find_free_run(std::uint32_t count,
                                        std::size_t preferred_subpart,
                                        std::uint32_t preferred_zone);
    [[nodiscard]] std::vector<ZoneRun> find_free_runs(std::uint32_t count,
                                                      std::size_t preferred_subpart,
                                                      std::uint32_t preferred_zone,
                                                      std::size_t max_runs);
    [[nodiscard]] bool commit_bitmap_changes(const std::vector<BitmapKey>& dirty,
                                             FileWriteResult& result,
                                             std::string_view label);
    void discard_bitmap_cache(const std::vector<BitmapKey>& dirty);

    [[nodiscard]] bool reserve_runs(std::span<const ZoneRun> runs,
                                    FileWriteResult& result,
                                    std::string_view label);
    [[nodiscard]] bool release_runs(std::span<const ZoneRun> runs,
                                    FileWriteResult& result,
                                    std::string_view label);
    [[nodiscard]] bool write_payload(const ZoneRun& run,
                                     std::span<const std::byte> bytes,
                                     std::size_t batch_bytes,
                                     FileWriteResult& result);
    [[nodiscard]] bool verify_payload(const ZoneRun& run,
                                      std::span<const std::byte> bytes,
                                      std::size_t batch_bytes);
    [[nodiscard]] bool write_payload_runs(std::span<const ZoneRun> runs,
                                          std::span<const std::byte> bytes,
                                          std::size_t batch_bytes,
                                          FileWriteResult& result);
    [[nodiscard]] bool verify_payload_runs(std::span<const ZoneRun> runs,
                                           std::span<const std::byte> bytes,
                                           std::size_t batch_bytes);
    [[nodiscard]] bool stage_inode(const Inode& inode, std::string_view label,
                                   FileWriteResult& result);
    [[nodiscard]] DirectorySlot plan_dentry_insert(const Node& parent,
                                                   std::string_view name,
                                                   const BlockInfo& inode_location);
    [[nodiscard]] DirectorySlot plan_directory_dentry_insert(
        const Node& parent,
        std::string_view name,
        const BlockInfo& inode_location);
    [[nodiscard]] bool publish_dentry(const DirectorySlot& slot,
                                      FileWriteResult& result);
    [[nodiscard]] bool verify_file(std::string_view path,
                                   std::span<const std::byte> expected);

    [[nodiscard]] bool grow_directory(Node& directory, FileWriteResult& result);
    [[nodiscard]] DentryLocation find_dentry(const Node& parent,
                                             std::string_view name);
    [[nodiscard]] FileWriteResult create_file_fragmented(
        Node parent, std::string_view full_path, std::string_view name,
        std::span<const std::byte> bytes, const FileWriteOptions& options);
    [[nodiscard]] FileWriteResult replace_file_fragmented(
        const Node& node, std::string_view full_path,
        std::span<const std::byte> bytes, const FileWriteOptions& options);
    [[nodiscard]] FileWriteResult create_file_segi(
        Node parent, std::string_view full_path, std::string_view name,
        std::span<const std::byte> bytes, const FileWriteOptions& options);
    [[nodiscard]] FileWriteResult replace_file_segi(
        const Node& node, std::string_view full_path,
        std::span<const std::byte> bytes, const FileWriteOptions& options);

    [[nodiscard]] DirectoryEnsureResult create_directory(const Node& parent,
                                                         std::string_view full_path,
                                                         std::string_view name,
                                                         const DirectoryWriteOptions& options);
    [[nodiscard]] FileWriteResult create_file(const Node& parent,
                                              std::string_view full_path,
                                              std::string_view name,
                                              std::span<const std::byte> bytes,
                                              const FileWriteOptions& options);
    [[nodiscard]] FileWriteResult replace_file(const Node& node,
                                               std::string_view full_path,
                                               std::span<const std::byte> bytes,
                                               const FileWriteOptions& options);

    WritableBlockDevice& device_;
    apa::Partition partition_;
    WritableApaVolume writable_volume_;
    ApaVolume read_volume_;
    ProbeResult probe_;
    std::string error_;
    std::map<BitmapKey, BitmapBytes> bitmap_cache_;
};

} // namespace ps2hdd::pfs
