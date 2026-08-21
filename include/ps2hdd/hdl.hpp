#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ps2hdd::hdl {

// HDLoader game metadata lives inside the main APA partition at +0x101000.
// The offsets below mirror the long-standing hdl-dump on-disk format, but the
// parser is independent and read-only. We read 1536 bytes rather than hdl-dump's
// historical 1024-byte buffer so a legal 1-main + 64-sub allocation table can be
// range-checked without reproducing its one-byte boundary overrun.
inline constexpr std::uint64_t kMetadataOffset = 0x101000ULL;
inline constexpr std::size_t kMetadataBytes = 1536;
inline constexpr std::uint32_t kMagic = 0xDEADFEEDU;
inline constexpr std::size_t kTitleOffset = 0x0008;
inline constexpr std::size_t kTitleStorage = 65; // 64-byte title + NUL
inline constexpr std::size_t kCompatOffset = 0x00A9;
inline constexpr std::size_t kDmaOffset = 0x00AA;
inline constexpr std::size_t kStartupOffset = 0x00AC;
inline constexpr std::size_t kStartupStorage = 13; // SXXX_000.00 + NUL
inline constexpr std::size_t kLayerBreakOffset = 0x00E8;
inline constexpr std::size_t kMediaOffset = 0x00EC;
inline constexpr std::size_t kPartCountOffset = 0x00F0;
inline constexpr std::size_t kAllocTableOffset = 0x00F5;
inline constexpr std::size_t kAllocEntryBytes = 12;
inline constexpr std::size_t kMaxAllocationEntries = 65;

static_assert(kAllocTableOffset + kMaxAllocationEntries * kAllocEntryBytes <= kMetadataBytes);

enum class MediaType {
    unknown,
    cd,
    dvd,
};

struct GameInfo {
    std::string partition_id;
    std::string title;
    std::string startup;
    std::uint32_t start_lba{};
    std::uint8_t compat_flags{};
    std::uint16_t dma{};
    std::uint32_t layer_break{};
    MediaType media{MediaType::unknown};
    std::uint8_t declared_parts{};
    std::uint64_t raw_size_bytes{};
    std::uint64_t allocated_size_bytes{};
    bool allocation_table_consistent{};
};

struct GameResult {
    bool ok{};
    std::string error;
    GameInfo game;
};

struct CatalogResult {
    std::vector<GameResult> entries;
    std::size_t readable{};
    std::size_t unreadable{};
};

[[nodiscard]] inline const char* media_type_name(MediaType media) noexcept
{
    switch (media) {
    case MediaType::cd:
        return "CD";
    case MediaType::dvd:
        return "DVD";
    case MediaType::unknown:
    default:
        return "unknown";
    }
}

namespace detail {

[[nodiscard]] inline std::uint16_t load_u16_le(const std::byte* p) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[0])) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[1])) << 8U);
}

[[nodiscard]] inline std::uint32_t load_u32_le(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

[[nodiscard]] inline std::string read_c_string(const std::byte* p, std::size_t storage,
                                               bool& terminated)
{
    std::string value;
    value.reserve(storage == 0 ? 0 : storage - 1);
    terminated = false;
    for (std::size_t i = 0; i < storage; ++i) {
        const auto byte = std::to_integer<unsigned char>(p[i]);
        if (byte == 0) {
            terminated = true;
            break;
        }
        value.push_back(static_cast<char>(byte));
    }
    return value;
}

[[nodiscard]] inline bool range_fits(std::uint64_t offset, std::uint64_t bytes,
                                     std::uint64_t size) noexcept
{
    return offset <= size && bytes <= size - offset;
}

} // namespace detail

[[nodiscard]] inline GameResult read_game_info(BlockDevice& device,
                                               const apa::Partition& partition)
{
    GameResult result;
    result.game.partition_id = partition.id;
    result.game.start_lba = partition.start_lba;
    result.game.allocated_size_bytes = partition.size_bytes();

    if (partition.is_sub() || partition.type != apa::kTypeHdl) {
        result.error = "APA partition is not an HDL main partition";
        return result;
    }

    const std::uint64_t main_bytes =
        static_cast<std::uint64_t>(partition.length_sectors) * apa::kSectorSize;
    if (!detail::range_fits(kMetadataOffset, kMetadataBytes, main_bytes)) {
        result.error = "HDL metadata header does not fit inside the main APA extent";
        return result;
    }

    const std::uint64_t partition_offset =
        static_cast<std::uint64_t>(partition.start_lba) * apa::kSectorSize;
    if (partition_offset > std::numeric_limits<std::uint64_t>::max() - kMetadataOffset) {
        result.error = "HDL metadata offset overflows host address space";
        return result;
    }
    const std::uint64_t metadata_offset = partition_offset + kMetadataOffset;
    if (!detail::range_fits(metadata_offset, kMetadataBytes, device.size_bytes())) {
        result.error = "HDL metadata header extends outside the backing device";
        return result;
    }

    std::array<std::byte, kMetadataBytes> bytes{};
    if (!device.read(metadata_offset, bytes)) {
        result.error = "Could not read HDL metadata header";
        return result;
    }
    if (detail::load_u32_le(bytes.data()) != kMagic) {
        result.error = "HDL metadata magic 0xDEADFEED is missing";
        return result;
    }

    bool title_terminated = false;
    result.game.title = detail::read_c_string(bytes.data() + kTitleOffset,
                                              kTitleStorage, title_terminated);
    if (!title_terminated) {
        result.error = "HDL title is not NUL-terminated within the format limit";
        return result;
    }

    bool startup_terminated = false;
    result.game.startup = detail::read_c_string(bytes.data() + kStartupOffset,
                                                kStartupStorage, startup_terminated);
    if (!startup_terminated) {
        result.error = "HDL startup ID is not NUL-terminated within the format limit";
        return result;
    }

    result.game.compat_flags = std::to_integer<std::uint8_t>(bytes[kCompatOffset]);
    result.game.dma = detail::load_u16_le(bytes.data() + kDmaOffset);
    result.game.layer_break = detail::load_u32_le(bytes.data() + kLayerBreakOffset);

    const auto media = detail::load_u32_le(bytes.data() + kMediaOffset);
    if (media == 0x14U) {
        result.game.media = MediaType::dvd;
    } else if (media == 0x12U) {
        result.game.media = MediaType::cd;
    }

    const auto declared = std::to_integer<std::uint8_t>(bytes[kPartCountOffset]);
    result.game.declared_parts = declared;
    if (declared == 0 || declared > kMaxAllocationEntries) {
        result.error = "HDL allocation-table part count is outside 1..65";
        return result;
    }

    std::uint64_t raw_units = 0;
    for (std::size_t i = 0; i < declared; ++i) {
        const std::size_t entry = kAllocTableOffset + i * kAllocEntryBytes;
        const auto length = detail::load_u32_le(bytes.data() + entry + 8);
        raw_units += length;
    }

    // hdl-dump exposes raw_size_in_kb as sum(length) / 4, which means each
    // allocation-table length unit represents 256 bytes.
    if (raw_units <= std::numeric_limits<std::uint64_t>::max() / 256ULL) {
        result.game.raw_size_bytes = raw_units * 256ULL;
    } else {
        result.error = "HDL raw-size calculation overflows";
        return result;
    }

    const std::uint32_t expected_parts = std::min<std::uint32_t>(
        partition.sub_count + 1U, static_cast<std::uint32_t>(kMaxAllocationEntries));
    result.game.allocation_table_consistent = declared == expected_parts;
    result.ok = true;
    return result;
}

// Build game metadata from the existing APA scan. Main HDL partitions are read
// in ascending physical LBA so a rotational disk does not bounce the heads in
// arbitrary UI/list order. SSDs are not harmed by this ordering, and future
// adaptive concurrency can be layered on top without changing the parser.
[[nodiscard]] inline CatalogResult read_catalog(BlockDevice& device,
                                                const apa::ScanResult& scan)
{
    std::vector<const apa::Partition*> partitions;
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.type == apa::kTypeHdl) {
            partitions.push_back(&partition);
        }
    }
    std::sort(partitions.begin(), partitions.end(), [](const auto* left, const auto* right) {
        return left->start_lba < right->start_lba;
    });

    CatalogResult catalog;
    catalog.entries.reserve(partitions.size());
    for (const auto* partition : partitions) {
        auto game = read_game_info(device, *partition);
        if (game.ok) {
            ++catalog.readable;
        } else {
            ++catalog.unreadable;
        }
        catalog.entries.emplace_back(std::move(game));
    }
    return catalog;
}

} // namespace ps2hdd::hdl
