#include "ps2hdd/disk_layout_guard.hpp"

#include <array>
#include <cstddef>
#include <cstring>

namespace ps2hdd {
namespace {

constexpr std::size_t kSectorBytes = 512;
constexpr std::size_t kMbrPartitionTable = 446;
constexpr std::size_t kMbrEntryBytes = 16;
constexpr std::size_t kMbrEntries = 4;
constexpr std::size_t kMbrSignature = 510;
constexpr std::size_t kPartitionTypeOffset = 4;
constexpr unsigned char kProtectiveGptType = 0xEE;
constexpr char kGptSignature[] = "EFI PART";

} // namespace

DiskLayoutGuardResult inspect_disk_layout(BlockDevice& device)
{
    DiskLayoutGuardResult result;
    if (device.size_bytes() < 2 * kSectorBytes) {
        result.error = "Device is too small for MBR/GPT admission sectors";
        return result;
    }

    std::array<std::byte, kSectorBytes> lba0{};
    std::array<std::byte, kSectorBytes> lba1{};
    if (!device.read(0, lba0) || !device.read(kSectorBytes, lba1)) {
        result.error = "Could not read MBR/GPT admission sectors";
        return result;
    }

    result.mbr_signature =
        std::to_integer<unsigned char>(lba0[kMbrSignature]) == 0x55U &&
        std::to_integer<unsigned char>(lba0[kMbrSignature + 1]) == 0xAAU;

    if (result.mbr_signature) {
        for (std::size_t index = 0; index < kMbrEntries; ++index) {
            const auto type = std::to_integer<unsigned char>(
                lba0[kMbrPartitionTable + index * kMbrEntryBytes + kPartitionTypeOffset]);
            if (type == kProtectiveGptType) {
                result.protective_entry = true;
            } else if (type != 0) {
                result.other_mbr_entries = true;
            }
        }
    }

    result.gpt_header = std::memcmp(lba1.data(), kGptSignature, sizeof(kGptSignature) - 1) == 0;

    if (result.protective_entry && result.other_mbr_entries) {
        result.kind = LegacyPartitionMapKind::hybrid_gpt;
    } else if (result.protective_entry) {
        result.kind = LegacyPartitionMapKind::protective_gpt;
    } else if (result.gpt_header) {
        result.kind = LegacyPartitionMapKind::gpt_header_without_protective_mbr;
    } else if (result.mbr_signature && result.other_mbr_entries) {
        result.kind = LegacyPartitionMapKind::legacy_mbr;
    }

    result.ok = true;
    return result;
}

const char* legacy_partition_map_name(LegacyPartitionMapKind kind) noexcept
{
    switch (kind) {
    case LegacyPartitionMapKind::none: return "none";
    case LegacyPartitionMapKind::legacy_mbr: return "legacy MBR";
    case LegacyPartitionMapKind::protective_gpt: return "protective GPT";
    case LegacyPartitionMapKind::hybrid_gpt: return "hybrid GPT/MBR";
    case LegacyPartitionMapKind::gpt_header_without_protective_mbr:
        return "GPT header without protective MBR";
    }
    return "unknown";
}

} // namespace ps2hdd
