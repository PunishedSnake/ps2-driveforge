#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace ps2hdd::hdl {

// HDLoader game metadata lives inside the main APA partition at +0x101000.
// We intentionally read 1536 bytes so the legal 1-main + 64-sub allocation
// table can be range-checked without reproducing hdl-dump's historical
// one-byte boundary overrun.
inline constexpr std::uint64_t kMetadataOffset = 0x101000ULL;
inline constexpr std::size_t kMetadataBytes = 1536;
inline constexpr std::uint32_t kMagic = 0xDEADFEEDU;
inline constexpr std::size_t kTitleOffset = 0x0008;
inline constexpr std::size_t kTitleStorage = 65;
inline constexpr std::size_t kCompatOffset = 0x00A9;
inline constexpr std::size_t kDmaOffset = 0x00AA;
inline constexpr std::size_t kStartupOffset = 0x00AC;
inline constexpr std::size_t kStartupStorage = 13;
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
    std::size_t total_candidates{};
    std::size_t readable{};
    std::size_t unreadable{};
    bool cancelled{};
};

using ProgressCallback =
    std::function<void(std::size_t completed, std::size_t total, const GameResult& game)>;

[[nodiscard]] const char* media_type_name(MediaType media) noexcept;

// Parse one native HDLoader metadata header. This is format code, not frontend
// enrichment policy: it performs one bounded read and never starts HDL.EXE.
[[nodiscard]] GameResult read_game_info(BlockDevice& device,
                                        const apa::Partition& partition);

// Build native game metadata from an existing APA scan in ascending physical
// LBA order. The callback is progressive/cancellable so callers can publish
// rows without coupling the parser to any GUI/threading framework.
[[nodiscard]] CatalogResult read_catalog(BlockDevice& device,
                                         const apa::ScanResult& scan,
                                         ProgressCallback progress = {},
                                         std::stop_token stop = {});

} // namespace ps2hdd::hdl
