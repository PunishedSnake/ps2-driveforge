#pragma once

#include "ps2hdd/block_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2hdd::apa {

inline constexpr std::uint32_t kMagic = 0x00415041U; // "APA\\0" on little-endian media
inline constexpr std::size_t kSectorSize = 512;
inline constexpr std::size_t kHeaderSize = 1024;
inline constexpr std::size_t kIdMax = 32;
inline constexpr std::size_t kPassMax = 8;
inline constexpr std::size_t kMaxSub = 64;
inline constexpr std::uint16_t kFlagSub = 0x0001;
inline constexpr std::uint16_t kTypeFree = 0x0000;
inline constexpr std::uint16_t kTypeMbr = 0x0001;
inline constexpr std::uint16_t kTypePfs = 0x0100;
inline constexpr std::uint16_t kTypeHdl = 0x1337;

#pragma pack(push, 1)
struct Ps2Time {
    std::uint8_t unused;
    std::uint8_t sec;
    std::uint8_t min;
    std::uint8_t hour;
    std::uint8_t day;
    std::uint8_t month;
    std::uint16_t year;
};

struct SubPartition {
    std::uint32_t start;
    std::uint32_t length;
};

struct MbrData {
    char magic[32];
    std::uint32_t version;
    std::uint32_t nsector;
    Ps2Time created;
    std::uint32_t osd_start;
    std::uint32_t osd_size;
    std::byte reserved[200];
};

struct Header {
    std::uint32_t checksum;
    std::uint32_t magic;
    std::uint32_t next;
    std::uint32_t prev;
    char id[kIdMax];
    char rpwd[kPassMax];
    char fpwd[kPassMax];
    std::uint32_t start;
    std::uint32_t length;
    std::uint16_t type;
    std::uint16_t flags;
    std::uint32_t nsub;
    Ps2Time created;
    std::uint32_t main;
    std::uint32_t number;
    std::uint32_t modver;
    std::uint32_t padding1[7];
    char padding2[128];
    MbrData mbr;
    SubPartition subs[kMaxSub];
};
#pragma pack(pop)

static_assert(sizeof(Ps2Time) == 8);
static_assert(sizeof(SubPartition) == 8);
static_assert(sizeof(MbrData) == 256);
static_assert(sizeof(Header) == kHeaderSize);

struct Partition {
    std::string id;
    std::uint32_t start_lba{};
    std::uint32_t length_sectors{};
    std::uint64_t total_sectors{};
    std::uint16_t type{};
    std::uint16_t flags{};
    std::uint32_t prev_lba{};
    std::uint32_t next_lba{};
    std::uint32_t main_lba{};
    std::uint32_t number{};
    std::uint32_t sub_count{};
    Ps2Time created{};
    std::vector<SubPartition> sub_partitions;

    [[nodiscard]] bool is_sub() const noexcept { return (flags & kFlagSub) != 0; }
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return total_sectors * kSectorSize; }
};

enum class IssueSeverity { warning, error };

struct Issue {
    IssueSeverity severity{IssueSeverity::error};
    std::uint32_t lba{};
    std::string message;
};

struct ScanResult {
    bool mbr_valid{};
    std::uint32_t apa_version{};
    std::vector<Partition> partitions;
    std::vector<Issue> issues;

    [[nodiscard]] bool ok() const noexcept;
};

[[nodiscard]] std::uint32_t checksum(const Header& header) noexcept;
[[nodiscard]] std::string type_name(std::uint16_t type);
[[nodiscard]] std::string partition_id(const Header& header);
[[nodiscard]] bool has_sony_mbr_magic(const Header& header) noexcept;

class Reader {
public:
    explicit Reader(BlockDevice& device) : device_(device) {}
    [[nodiscard]] ScanResult scan(std::size_t max_headers = 65536);

private:
    [[nodiscard]] bool read_header(std::uint32_t lba, Header& out);
    BlockDevice& device_;
};

} // namespace ps2hdd::apa
