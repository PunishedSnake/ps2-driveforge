#pragma once

#include "ps2hdd/block_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

// APA-Jail (Berion; used by the PSBBN Definitive Project): a PC partition
// table shares sector 0 with the APA __mbr header so APA and exFAT coexist on
// one drive. Type A2 writes "APAJ-A2" at 0xE0 and an MBR table at 0x1BE:
//   #1 type 0x17  the APA region (APA partitions must stay inside it)
//   #2            32 MiB ext2 RECOVERY (APA index + GPT tail backups)
//   #3 type 0x07  exFAT "OPL" for games and apps
// The APA header checksum covers these bytes, so every APA header rewrite
// must preserve them (DriveForge rewrites whole headers read from disk).
struct HybridMbrEntry {
    std::uint8_t type{};
    std::uint32_t start_lba{};
    std::uint32_t sectors{};
};

inline constexpr std::size_t kJailSignatureOffset = 0xE0;
inline constexpr std::size_t kMbrTableOffset = 0x1BE;

struct JailInfo {
    std::string signature;                 // e.g. "APAJ-A2"; empty for an unsigned hybrid
    std::vector<HybridMbrEntry> entries;   // non-empty MBR entries in table order
    bool gpt_protective{};                 // GPT-based variant (0xEE)
    // First LBA APA may not use. 0 = unknown -> APA allocation fails closed.
    std::uint64_t apa_limit_lba{};
    std::optional<HybridMbrEntry> exfat;
    std::optional<HybridMbrEntry> recovery;
};

struct ScanResult {
    bool mbr_valid{};
    std::uint32_t apa_version{};
    std::vector<Partition> partitions;
    std::vector<Issue> issues;
    std::optional<JailInfo> jail;

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
