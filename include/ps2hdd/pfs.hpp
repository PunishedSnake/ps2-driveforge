#pragma once

#include "ps2hdd/apa_volume.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ps2hdd::pfs {

inline constexpr std::uint32_t kSuperMagic = 0x50465300U;
inline constexpr std::uint32_t kFormatVersion = 3;
inline constexpr std::uint32_t kSuperSector = 8192;
inline constexpr std::uint32_t kSuperBackupSector = 8193;
inline constexpr std::uint32_t kFsckWriteError = 0x01;
inline constexpr std::uint32_t kFsckErrorsFixed = 0x02;

#pragma pack(push, 1)
struct BlockInfo {
    std::uint32_t number;
    std::uint16_t subpart;
    std::uint16_t count;
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
#pragma pack(pop)

static_assert(sizeof(BlockInfo) == 8);
static_assert(sizeof(SuperBlock) == 40);

struct ProbeResult {
    bool valid{};
    bool backup_matches{};
    SuperBlock super{};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

[[nodiscard]] bool valid_zone_size(std::uint32_t zone_size) noexcept;
[[nodiscard]] ProbeResult probe(ApaVolume& volume);

} // namespace ps2hdd::pfs
