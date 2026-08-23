#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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

// Frieren's first writable HDL operation is deliberately non-structural: it
// patches fields inside an already valid HDL metadata header. Partition layout,
// allocation-table entries, startup ID, media type and payload extents remain
// untouched. Empty optionals mean "leave the existing value unchanged".
struct MetadataPatch {
    std::optional<std::string> title;
    std::optional<std::uint8_t> compat_flags;
    std::optional<std::uint16_t> dma;

    [[nodiscard]] bool empty() const noexcept
    {
        return !title.has_value() && !compat_flags.has_value() && !dma.has_value();
    }
};

using ProgressCallback =
    std::function<void(std::size_t completed, std::size_t total, const GameResult& game)>;

[[nodiscard]] const char* media_type_name(MediaType media) noexcept;

// Parse one native HDLoader metadata header. This is format code, not frontend
// enrichment policy: it performs one bounded read and never starts HDL.EXE.
[[nodiscard]] GameResult read_game_info(BlockDevice& device,
                                        const apa::Partition& partition);

// Patch selected user-facing fields in one existing HDL metadata header. The
// function validates the current header first, performs one fixed-size in-place
// write, flushes it, reads the bytes back, and finally reparses them through the
// normal reader before reporting success.
[[nodiscard]] GameResult patch_game_metadata(WritableBlockDevice& device,
                                             const apa::Partition& partition,
                                             const MetadataPatch& patch);

// Build native game metadata from an existing APA scan in ascending physical
// LBA order. The callback is progressive/cancellable so callers can publish
// rows without coupling the parser to any GUI/threading framework.
[[nodiscard]] CatalogResult read_catalog(BlockDevice& device,
                                         const apa::ScanResult& scan,
                                         ProgressCallback progress = {},
                                         std::stop_token stop = {});

} // namespace ps2hdd::hdl
