#pragma once

#include "ps2hdd/apa_volume.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps2hdd::pfs {

inline constexpr std::uint32_t kSuperMagic = 0x50465300U;
inline constexpr std::uint32_t kJournalMagic = 0x5046534CU;
inline constexpr std::uint32_t kSegdMagic = 0x53454744U;
inline constexpr std::uint32_t kSegiMagic = 0x53454749U;
inline constexpr std::uint32_t kFormatVersion = 3;
inline constexpr std::uint32_t kSuperSector = 8192;
inline constexpr std::uint32_t kSuperBackupSector = 8193;
inline constexpr std::uint32_t kFsckWriteError = 0x01;
inline constexpr std::uint32_t kFsckErrorsFixed = 0x02;
inline constexpr std::size_t kMetadataSize = 1024;
inline constexpr std::size_t kInodeMaxBlocks = 114;
inline constexpr std::size_t kIndirectMaxBlocks = 123;
inline constexpr std::uint16_t kModeMask = 0xF000;
inline constexpr std::uint16_t kModeDirectory = 0x1000;
inline constexpr std::uint16_t kModeRegular = 0x2000;

#pragma pack(push, 1)
struct BlockInfo {
    std::uint32_t number;
    std::uint16_t subpart;
    std::uint16_t count;
};

struct DateTime {
    std::uint8_t unused;
    std::uint8_t sec;
    std::uint8_t min;
    std::uint8_t hour;
    std::uint8_t day;
    std::uint8_t month;
    std::uint16_t year;
};

struct SuperBlock {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t modver;
    std::uint32_t fsck_stat;
    std::uint32_t zone_size;
    std::uint32_t num_subs;
    BlockInfo log;
    BlockInfo root;
};

struct Inode {
    std::uint32_t checksum;
    std::uint32_t magic;
    BlockInfo inode_block;
    BlockInfo next_segment;
    BlockInfo last_segment;
    BlockInfo unused;
    BlockInfo data[kInodeMaxBlocks];
    std::uint16_t mode;
    std::uint16_t attr;
    std::uint16_t uid;
    std::uint16_t gid;
    DateTime atime;
    DateTime ctime;
    DateTime mtime;
    std::uint64_t size;
    std::uint32_t number_blocks;
    std::uint32_t number_data;
    std::uint32_t number_segdesg;
    std::uint32_t subpart;
    std::uint32_t reserved[4];
};

// PFS deliberately reuses the 72-byte metadata tail of indirect SEGI records
// as nine additional BlockInfo entries. 40-byte header + 123 * 8 bytes = 1024.
struct SegmentDescriptor {
    std::uint32_t checksum;
    std::uint32_t magic;
    BlockInfo inode_block;
    BlockInfo next_segment;
    BlockInfo last_segment;
    BlockInfo unused;
    BlockInfo data[kIndirectMaxBlocks];
};
#pragma pack(pop)

static_assert(sizeof(BlockInfo) == 8);
static_assert(sizeof(DateTime) == 8);
static_assert(sizeof(SuperBlock) == 40);
static_assert(sizeof(Inode) == kMetadataSize);
static_assert(sizeof(SegmentDescriptor) == kMetadataSize);

struct ProbeResult {
    bool valid{};
    bool backup_matches{};
    SuperBlock super{};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

struct DirectoryEntry {
    std::string name;
    BlockInfo inode{};
    std::uint16_t mode{};

    [[nodiscard]] bool is_directory() const noexcept { return (mode & kModeMask) == kModeDirectory; }
    [[nodiscard]] bool is_regular() const noexcept { return (mode & kModeMask) == kModeRegular; }
};

struct Node {
    BlockInfo location{};
    Inode inode{};
};

[[nodiscard]] bool valid_zone_size(std::uint32_t zone_size) noexcept;
[[nodiscard]] ProbeResult probe(ApaVolume& volume);
[[nodiscard]] std::uint32_t inode_checksum(const Inode& inode) noexcept;
[[nodiscard]] std::uint32_t segment_checksum(const SegmentDescriptor& descriptor) noexcept;

class Reader {
public:
    explicit Reader(ApaVolume& volume);

    // Read-only frontends may reuse a ProbeResult already validated against the
    // same immutable APA volume. This avoids rereading primary/backup superblocks
    // for every Explorer stat/read without weakening the normal probing API.
    Reader(ApaVolume& volume, ProbeResult validated_probe)
        : volume_(volume), probe_(std::move(validated_probe))
    {
        if (!probe_.valid && !probe_.errors.empty()) {
            last_error_ = probe_.errors.front();
        }
    }

    [[nodiscard]] bool valid() const noexcept { return probe_.valid; }
    [[nodiscard]] const ProbeResult& probe_result() const noexcept { return probe_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

    [[nodiscard]] std::optional<Node> root();
    [[nodiscard]] std::optional<Node> read_inode(BlockInfo location);
    [[nodiscard]] std::optional<Node> resolve(std::string_view path);
    [[nodiscard]] std::vector<DirectoryEntry> list_directory(const Node& directory,
                                                              bool include_dot_entries = false);

    // Reads any byte range described by the inode, following both direct SEGD
    // block descriptors and chained indirect SEGI descriptors.
    bool read(const Node& node, std::uint64_t offset, std::span<std::byte> out);

private:
    [[nodiscard]] unsigned inode_scale() const noexcept;
    [[nodiscard]] std::optional<SegmentDescriptor> read_segment_descriptor(BlockInfo location);
    bool read_zone_bytes(const BlockInfo& block, std::uint64_t zone_offset,
                         std::span<std::byte> out);
    void fail(std::string message);

    ApaVolume& volume_;
    ProbeResult probe_;
    std::string last_error_;
};

} // namespace ps2hdd::pfs
