#pragma once

#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd::pfs {

inline constexpr std::uint32_t kSegdMagic = 0x53454744U; // "SEGD"
inline constexpr std::uint32_t kSegiMagic = 0x53454749U; // "SEGI"
inline constexpr std::size_t kMetaSize = 1024;
inline constexpr std::size_t kDirectBlockInfos = 114;
inline constexpr std::size_t kIndirectBlockInfos = 123;
inline constexpr std::uint16_t kModeTypeMask = 0xF000;
inline constexpr std::uint16_t kModeDirectory = 0x1000;
inline constexpr std::uint16_t kModeRegular = 0x2000;
inline constexpr std::uint16_t kModeSymlink = 0x4000;

#pragma pack(push, 1)
struct DateTime {
    std::uint8_t unused;
    std::uint8_t sec;
    std::uint8_t min;
    std::uint8_t hour;
    std::uint8_t day;
    std::uint8_t month;
    std::uint16_t year;
};

struct Inode {
    std::uint32_t checksum;
    std::uint32_t magic;
    BlockInfo inode_block;
    BlockInfo next_segment;
    BlockInfo last_segment;
    BlockInfo unused;
    BlockInfo data[kDirectBlockInfos];
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

struct DirectoryEntryHeader {
    std::uint32_t inode;
    std::uint8_t sub;
    std::uint8_t path_length;
    std::uint16_t allocated_length;
};
#pragma pack(pop)

static_assert(sizeof(DateTime) == 8);
static_assert(sizeof(Inode) == kMetaSize);
static_assert(sizeof(DirectoryEntryHeader) == 8);

struct Node {
    BlockInfo location{};
    Inode inode{};

    [[nodiscard]] bool is_directory() const noexcept
    {
        return (inode.mode & kModeTypeMask) == kModeDirectory;
    }
    [[nodiscard]] bool is_regular() const noexcept
    {
        return (inode.mode & kModeTypeMask) == kModeRegular;
    }
};

struct DirectoryEntry {
    std::string name;
    BlockInfo location{};
    std::uint16_t type{};

    [[nodiscard]] bool is_directory() const noexcept { return type == kModeDirectory; }
    [[nodiscard]] bool is_regular() const noexcept { return type == kModeRegular; }
};

struct DirectoryResult {
    std::vector<DirectoryEntry> entries;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;

    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

class FileSystem {
public:
    explicit FileSystem(ApaVolume& volume) : volume_(volume) {}

    [[nodiscard]] bool mount();
    [[nodiscard]] bool mounted() const noexcept { return mounted_; }
    [[nodiscard]] const ProbeResult& probe_result() const noexcept { return probe_; }
    [[nodiscard]] const SuperBlock& superblock() const noexcept { return probe_.super; }

    [[nodiscard]] bool read_inode(const BlockInfo& location, Inode& out,
                                  std::string* error = nullptr);
    [[nodiscard]] DirectoryResult list_directory(const BlockInfo& location,
                                                 bool include_special = false);
    [[nodiscard]] std::optional<Node> resolve(std::string_view path,
                                              std::string* error = nullptr);

    // Reads up to out.size() bytes. bytes_read may be smaller at EOF.
    [[nodiscard]] bool read_file(const Node& node, std::uint64_t offset,
                                 std::span<std::byte> out, std::size_t& bytes_read,
                                 std::string* error = nullptr);

private:
    struct RawInode {
        Inode parsed{};
        std::array<std::byte, kMetaSize> bytes{};
    };

    [[nodiscard]] bool read_raw_inode(const BlockInfo& location, std::uint32_t expected_magic,
                                      RawInode& out, std::string* error);
    [[nodiscard]] bool collect_data_extents(const Inode& inode, std::vector<BlockInfo>& extents,
                                            std::string* error);
    [[nodiscard]] bool read_inode_data(const Inode& inode, std::uint64_t offset,
                                       std::span<std::byte> out, std::size_t& bytes_read,
                                       std::string* error);
    [[nodiscard]] bool read_volume_bytes(std::uint16_t subpart, std::uint64_t byte_offset,
                                         std::span<std::byte> out, std::string* error);

    ApaVolume& volume_;
    ProbeResult probe_{};
    bool mounted_{};
    std::uint32_t zone_sectors_{};
};

[[nodiscard]] std::uint32_t inode_checksum(const Inode& inode) noexcept;

} // namespace ps2hdd::pfs
