#include "ps2hdd/hdl.hpp"
#include "ps2hdd/write_transaction.hpp"

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

void store_u16_le(std::byte* p, std::uint16_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
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

[[nodiscard]] bool metadata_offset_for(const BlockDevice& device,
                                       const apa::Partition& partition,
                                       std::uint64_t& out) noexcept
{
    const std::uint64_t main_bytes =
        static_cast<std::uint64_t>(partition.length_sectors) * apa::kSectorSize;
    if (!range_fits(kMetadataOffset, kMetadataBytes, main_bytes)) {
        return false;
    }

    const std::uint64_t partition_offset =
        static_cast<std::uint64_t>(partition.start_lba) * apa::kSectorSize;
    if (partition_offset > std::numeric_limits<std::uint64_t>::max() - kMetadataOffset) {
        return false;
    }

    out = partition_offset + kMetadataOffset;
    return range_fits(out, kMetadataBytes, device.size_bytes());
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

namespace {

// Reads and validates the fixed header. On success `bytes` holds the complete
// metadata window so callers can decode further fields without a second read.
GameResult read_game_info_bytes(BlockDevice& device, const apa::Partition& partition,
                                std::array<std::byte, kMetadataBytes>& bytes)
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

struct OwnedExtent {
    std::uint64_t data_begin{};
    std::uint64_t end{};
};

} // namespace

GameResult read_game_info(BlockDevice& device, const apa::Partition& partition)
{
    std::array<std::byte, kMetadataBytes> bytes{};
    return read_game_info_bytes(device, partition, bytes);
}

LayoutResult read_game_layout(BlockDevice& device, const apa::Partition& partition)
{
    LayoutResult result;
    std::array<std::byte, kMetadataBytes> bytes{};
    auto info = read_game_info_bytes(device, partition, bytes);
    if (!info.ok) {
        result.error = std::move(info.error);
        return result;
    }
    result.layout.game = std::move(info.game);

    // Byte ranges this game is allowed to store data in.
    std::vector<OwnedExtent> owned;
    owned.reserve(partition.sub_partitions.size() + 1);
    const auto add_owned = [&](std::uint64_t start_lba, std::uint64_t sectors,
                               std::uint64_t reserve) {
        const std::uint64_t begin = start_lba * apa::kSectorSize;
        const std::uint64_t end = begin + sectors * apa::kSectorSize;
        owned.push_back({std::min(end, begin + reserve), end});
    };
    add_owned(partition.start_lba, partition.length_sectors, kMainDataReserveBytes);
    for (const auto& sub : partition.sub_partitions) {
        add_owned(sub.start, sub.length, kSubDataReserveBytes);
    }

    const auto declared = result.layout.game.declared_parts;
    auto& extents = result.layout.extents;
    extents.reserve(declared);
    std::uint64_t iso_cursor = 0;
    for (std::size_t i = 0; i < declared; ++i) {
        const std::byte* record = bytes.data() + kPartSpecsOffset + i * kPartSpecBytes;
        const std::uint64_t part_offset = load_u32_le(record);
        const std::uint64_t data_start = load_u32_le(record + 4);
        const std::uint64_t part_size = load_u32_le(record + 8);
        const std::string where = "HDL allocation slice " + std::to_string(i) + ": ";

        if (part_size == 0 || part_size % kIsoSectorSize != 0) {
            result.error = where + "size is not a positive multiple of 2048 bytes";
            return result;
        }
        if (part_offset * kIsoSectorSize != iso_cursor) {
            result.error = where + "ISO offset is not contiguous with the previous slice";
            return result;
        }

        const std::uint64_t device_offset = data_start * apa::kSectorSize;
        const std::uint64_t device_end = device_offset + part_size;
        if (device_end > device.size_bytes()) {
            result.error = where + "extends outside the backing device";
            return result;
        }

        std::uint32_t owner = 0;
        bool inside = false;
        for (std::size_t k = 0; k < owned.size(); ++k) {
            if (device_offset >= owned[k].data_begin && device_end <= owned[k].end) {
                owner = static_cast<std::uint32_t>(k);
                inside = true;
                break;
            }
        }
        if (!inside) {
            result.error = where + "is not inside a data area owned by this game's APA partitions";
            return result;
        }

        for (const auto& previous : extents) {
            const auto prev_end = previous.device_offset + previous.length;
            if (device_offset < prev_end && previous.device_offset < device_end) {
                result.error = where + "overlaps another slice on disk";
                return result;
            }
        }

        extents.push_back({iso_cursor, device_offset, part_size, owner});
        iso_cursor += part_size;
    }

    result.layout.iso_size_bytes = iso_cursor;
    result.ok = true;
    return result;
}

IsoReadResult read_iso(BlockDevice& device, const GameLayout& layout, std::uint64_t offset,
                       std::span<std::byte> out)
{
    IsoReadResult result;
    if (offset >= layout.iso_size_bytes || out.empty()) {
        result.ok = true;
        return result;
    }
    const std::uint64_t wanted =
        std::min<std::uint64_t>(out.size(), layout.iso_size_bytes - offset);

    // First extent whose range ends after `offset`.
    auto it = std::upper_bound(layout.extents.begin(), layout.extents.end(), offset,
                               [](std::uint64_t value, const IsoExtent& extent) {
                                   return value < extent.iso_offset + extent.length;
                               });
    std::uint64_t done = 0;
    while (done < wanted && it != layout.extents.end()) {
        const std::uint64_t position = offset + done;
        const std::uint64_t inside = position - it->iso_offset;
        const std::uint64_t chunk = std::min(wanted - done, it->length - inside);
        if (!device.read(it->device_offset + inside,
                         out.subspan(static_cast<std::size_t>(done),
                                     static_cast<std::size_t>(chunk)))) {
            result.error = "Could not read HDL game data from the backing device";
            result.bytes_read = static_cast<std::size_t>(done);
            return result;
        }
        done += chunk;
        ++it;
    }
    result.bytes_read = static_cast<std::size_t>(done);
    result.ok = done == wanted;
    if (!result.ok) {
        result.error = "HDL layout ended before the declared ISO size";
    }
    return result;
}

GameResult patch_game_metadata(WritableBlockDevice& device,
                               const apa::Partition& partition,
                               const MetadataPatch& patch)
{
    if (patch.empty()) {
        GameResult result;
        result.error = "No HDL metadata fields were requested for update";
        return result;
    }
    if (patch.title.has_value() && patch.title->size() >= kTitleStorage) {
        GameResult result;
        result.error = "HDL title exceeds the 64-byte format limit";
        return result;
    }

    const auto current = read_game_info(device, partition);
    if (!current.ok) {
        GameResult result;
        result.error = "Refusing to patch invalid HDL metadata: " + current.error;
        return result;
    }

    std::uint64_t metadata_offset = 0;
    if (!metadata_offset_for(device, partition, metadata_offset)) {
        GameResult result;
        result.error = "HDL metadata write range is outside the main APA extent or backing device";
        return result;
    }

    std::array<std::byte, kMetadataBytes> original{};
    if (!device.read(metadata_offset, original)) {
        GameResult result;
        result.error = "Could not capture HDL metadata before-image";
        return result;
    }

    auto modified = original;
    if (patch.title.has_value()) {
        std::fill_n(modified.begin() + static_cast<std::ptrdiff_t>(kTitleOffset),
                    kTitleStorage, std::byte{0});
        for (std::size_t i = 0; i < patch.title->size(); ++i) {
            modified[kTitleOffset + i] =
                static_cast<std::byte>(static_cast<unsigned char>((*patch.title)[i]));
        }
    }
    if (patch.compat_flags.has_value()) {
        modified[kCompatOffset] = static_cast<std::byte>(*patch.compat_flags);
    }
    if (patch.dma.has_value()) {
        store_u16_le(modified.data() + kDmaOffset, *patch.dma);
    }

    WriteTransaction transaction(device);
    if (!transaction.stage(metadata_offset, modified, "HDL metadata")) {
        GameResult result;
        result.error = "Could not stage HDL metadata mutation: " + transaction.stage_error();
        return result;
    }

    GameResult verified;
    const auto commit = transaction.commit([&]() -> std::string {
        verified = read_game_info(device, partition);
        if (!verified.ok) {
            return "normal HDL parser rejected the written header: " + verified.error;
        }
        if (patch.title.has_value() && verified.game.title != *patch.title) {
            return "HDL title did not round-trip";
        }
        if (patch.compat_flags.has_value() && verified.game.compat_flags != *patch.compat_flags) {
            return "HDL compatibility flags did not round-trip";
        }
        if (patch.dma.has_value() && verified.game.dma != *patch.dma) {
            return "HDL DMA mode did not round-trip";
        }
        return {};
    });

    if (!commit.ok) {
        GameResult result;
        result.error = commit.error;
        return result;
    }
    return verified;
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
