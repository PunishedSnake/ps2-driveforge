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

// Image-only PFS mutation session. It deliberately owns a writable APA extent
// capability and never accepts PhysicalDrive. The first implementation targets
// OPL-sized regular files and is optimized around three properties:
//
//  * one PFS probe per session instead of shelling out / rebuilding a catalog;
//  * lazy bitmap chunks cached for the session instead of rescanning the whole FS;
//  * crash-friendly publication order: reserve zones -> payload -> inode -> dentry.
//
// Existing files are replaced copy-on-write. Old data zones are released only
// after the new inode has been committed and verified, so a failed cleanup can
// leak space but cannot make the live file point at zones already marked free.
//
// Current bounded scope: parent directories must already exist and contain a
// reusable dentry slot; indirect SEGI file layouts and directory growth are
// refused rather than guessed. Those become the next writer milestones.
class ImageWriter final {
public:
    using BitmapKey = std::pair<std::size_t, std::uint32_t>;
    using BitmapBytes = std::array<std::byte, kMetadataSize>;

    ImageWriter(WritableBlockDevice& device, const apa::Partition& partition);

    [[nodiscard]] bool valid() const noexcept { return probe_.valid; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const ProbeResult& probe_result() const noexcept { return probe_; }

    [[nodiscard]] FileWriteResult write_file(std::string_view path,
                                             std::span<const std::byte> bytes,
                                             const FileWriteOptions& options = {});

private:
    struct ZoneRun {
        std::size_t subpart{};
        std::uint32_t first{};
        std::uint32_t count{};
    };

    struct DirectorySlot {
        bool ok{};
        std::string error;
        std::size_t subpart{};
        std::uint32_t sector{};
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
    [[nodiscard]] bool commit_bitmap_changes(const std::vector<BitmapKey>& dirty,
                                             FileWriteResult& result,
                                             std::string_view label);
    void discard_bitmap_cache(const std::vector<BitmapKey>& dirty);

    [[nodiscard]] bool write_payload(const ZoneRun& run,
                                     std::span<const std::byte> bytes,
                                     std::size_t batch_bytes,
                                     FileWriteResult& result);
    [[nodiscard]] bool verify_payload(const ZoneRun& run,
                                      std::span<const std::byte> bytes,
                                      std::size_t batch_bytes);
    [[nodiscard]] bool stage_inode(const Inode& inode, std::string_view label,
                                   FileWriteResult& result);
    [[nodiscard]] DirectorySlot plan_dentry_insert(const Node& parent,
                                                   std::string_view name,
                                                   const BlockInfo& inode_location);
    [[nodiscard]] bool publish_dentry(const DirectorySlot& slot,
                                      FileWriteResult& result);
    [[nodiscard]] bool verify_file(std::string_view path,
                                   std::span<const std::byte> expected);

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
