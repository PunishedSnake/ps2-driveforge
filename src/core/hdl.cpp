#include "ps2hdd/hdl.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace ps2hdd::hdl {
namespace {

[[nodiscard]] std::uint16_t load_u16_le(const std::byte* p) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[0])) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[1])) << 8U);
}

[[nodiscard]] std::uint32_t load_u32_le(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

[[nodiscard]] std::string read_c_string(const std::byte* p, std::size_t storage,
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

[[nodiscard]] bool range_fits(std::uint64_t offset, std::uint64_t bytes,
                              std::uint64_t size) noexcept
{
    return offset <= size && bytes <= size - offset;
}

} // namespace

const char* media_type_name(MediaType media) noexcept
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

GameResult read_game_info(BlockDevice& device, const apa::Partition& partition)
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
    if (!range_fits(kMetadataOffset, kMetadataBytes, main_bytes)) {
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
    if (!range_fits(metadata_offset, kMetadataBytes, device.size_bytes())) {
        result.error = "HDL metadata header extends outside the backing device";
        return result;
    }

    // HDLoader stores a fixed metadata header at main-partition +0x101000. Read
    // the complete 1536-byte window once so all declared allocation entries can
    // be validated before any field is exposed to a frontend.
    std::array<std::byte, kMetadataBytes> bytes{};
    if (!device.read(metadata_offset, bytes)) {
        result.error = "Could not read HDL metadata header";
        return result;
    }
    if (load_u32_le(bytes.data()) != kMagic) {
        result.error = "HDL metadata magic 0xDEADFEED is missing";
        return result;
    }

    bool title_terminated = false;
    result.game.title = read_c_string(bytes.data() + kTitleOffset,
                                      kTitleStorage, title_terminated);
    if (!title_terminated) {
        result.error = "HDL title is not NUL-terminated within the format limit";
        return result;
    }

    bool startup_terminated = false;
    result.game.startup = read_c_string(bytes.data() + kStartupOffset,
                                        kStartupStorage, startup_terminated);
    if (!startup_terminated) {
        result.error = "HDL startup ID is not NUL-terminated within the format limit";
        return result;
    }

    result.game.compat_flags = std::to_integer<std::uint8_t>(bytes[kCompatOffset]);
    result.game.dma = load_u16_le(bytes.data() + kDmaOffset);
    result.game.layer_break = load_u32_le(bytes.data() + kLayerBreakOffset);

    const auto media = load_u32_le(bytes.data() + kMediaOffset);
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
        raw_units += load_u32_le(bytes.data() + entry + 8);
    }

    // hdl-dump exposes raw_size_in_kb as sum(length) / 4, therefore one
    // allocation-table length unit represents 256 bytes on disk.
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

CatalogResult read_catalog(BlockDevice& device, const apa::ScanResult& scan,
                           ProgressCallback progress, std::stop_token stop)
{
    std::vector<const apa::Partition*> partitions;
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.type == apa::kTypeHdl) {
            partitions.push_back(&partition);
        }
    }

    // Physical-LBA order is the safe baseline for rotational and unknown media:
    // it avoids arbitrary head bouncing while remaining harmless for SSDs.
    std::sort(partitions.begin(), partitions.end(), [](const auto* left, const auto* right) {
        return left->start_lba < right->start_lba;
    });

    CatalogResult catalog;
    catalog.total_candidates = partitions.size();
    catalog.entries.reserve(partitions.size());
    for (const auto* partition : partitions) {
        if (stop.stop_requested()) {
            catalog.cancelled = true;
            break;
        }

        auto game = read_game_info(device, *partition);
        if (game.ok) {
            ++catalog.readable;
        } else {
            ++catalog.unreadable;
        }
        catalog.entries.emplace_back(std::move(game));
        if (progress) {
            progress(catalog.entries.size(), catalog.total_candidates, catalog.entries.back());
        }
    }
    return catalog;
}

} // namespace ps2hdd::hdl
