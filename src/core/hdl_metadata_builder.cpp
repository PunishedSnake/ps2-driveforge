#include "ps2hdd/hdl_metadata_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace ps2hdd::hdl {
namespace {

inline constexpr std::uint32_t kMainReservedSectors = 0x2000; // 4 MiB
inline constexpr std::uint32_t kSubReservedSectors = 0x0800;  // 1 MiB
inline constexpr std::uint64_t kOffsetUnitBytes = 512ULL * 1024ULL;
inline constexpr std::uint64_t kLengthUnitBytes = 256ULL;

void store_u16_le(std::byte* p, std::uint16_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void store_u32_le(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

[[nodiscard]] bool startup_shape(const std::string& startup) noexcept
{
    if (startup.size() != 11 || (startup[4] != '_' && startup[4] != '-') || startup[8] != '.') {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (startup[i] < 'A' || startup[i] > 'Z') {
            return false;
        }
    }
    for (const std::size_t i : {5U, 6U, 7U, 9U, 10U}) {
        if (startup[i] < '0' || startup[i] > '9') {
            return false;
        }
    }
    return true;
}

} // namespace

MetadataBuildResult build_install_metadata(const apa::AllocationPlan& allocation,
                                           const MetadataBuildRequest& request)
{
    MetadataBuildResult result;
    if (!allocation.ok || allocation.extents.empty()) {
        result.error = "Cannot build HDL metadata from an invalid allocation plan";
        return result;
    }
    if (allocation.extents.size() > kMaxAllocationEntries) {
        result.error = "HDL allocation exceeds the 65-entry metadata table";
        return result;
    }
    if (request.title.empty() || request.title.size() >= kTitleStorage) {
        result.error = "HDL title must contain 1..64 bytes";
        return result;
    }
    if (!startup_shape(request.startup)) {
        result.error = "HDL startup ID is not in XXXX_000.00 form";
        return result;
    }
    if (request.payload_bytes == 0 || request.payload_bytes % 2048ULL != 0) {
        result.error = "HDL game payload must be a non-zero multiple of 2048 bytes";
        return result;
    }
    if (request.payload_bytes > allocation.usable_payload_bytes) {
        result.error = "HDL game payload exceeds planned usable allocation capacity";
        return result;
    }
    if (request.media != MediaType::cd && request.media != MediaType::dvd) {
        result.error = "HDL install metadata requires an explicit CD or DVD media type";
        return result;
    }

    store_u32_le(result.metadata.data(), kMagic);
    // Native HDLoader headers observed in existing tooling set this format byte.
    result.metadata[6] = std::byte{0x01};
    std::memcpy(result.metadata.data() + kTitleOffset, request.title.data(), request.title.size());
    result.metadata[kCompatOffset] = static_cast<std::byte>(request.compat_flags);
    store_u16_le(result.metadata.data() + kDmaOffset, request.dma);
    std::memcpy(result.metadata.data() + kStartupOffset, request.startup.data(), request.startup.size());
    store_u32_le(result.metadata.data() + kLayerBreakOffset, request.layer_break);
    store_u32_le(result.metadata.data() + kMediaOffset,
                 request.media == MediaType::dvd ? 0x14U : 0x12U);

    std::uint64_t remaining = request.payload_bytes;
    std::uint64_t source_offset = 0;
    for (std::size_t i = 0; i < allocation.extents.size() && remaining != 0; ++i) {
        const auto& extent = allocation.extents[i];
        const auto reserved = i == 0 ? kMainReservedSectors : kSubReservedSectors;
        if (extent.length_sectors <= reserved) {
            result.error = "Planned APA extent is smaller than its HDL reserved prefix";
            return result;
        }

        const std::uint64_t usable =
            static_cast<std::uint64_t>(extent.length_sectors - reserved) * apa::kSectorSize;
        const std::uint64_t take = std::min(remaining, usable);
        if (take == 0 || take % kLengthUnitBytes != 0 ||
            source_offset % kOffsetUnitBytes != 0) {
            result.error = "HDL allocation cannot be represented in metadata table units";
            return result;
        }

        const std::uint64_t data_start_sector =
            static_cast<std::uint64_t>(extent.start_lba) + reserved;
        if (data_start_sector % 256ULL != 0) {
            result.error = "HDL payload start is not aligned to metadata start units";
            return result;
        }

        const std::uint64_t offset_units = source_offset / kOffsetUnitBytes;
        const std::uint64_t start_units = data_start_sector >> 8U;
        const std::uint64_t length_units = take / kLengthUnitBytes;
        if (offset_units > std::numeric_limits<std::uint32_t>::max() ||
            start_units > std::numeric_limits<std::uint32_t>::max() ||
            length_units > std::numeric_limits<std::uint32_t>::max()) {
            result.error = "HDL allocation-table field exceeds its 32-bit on-disk representation";
            return result;
        }

        const std::size_t entry = kAllocTableOffset + result.payload_extents.size() * kAllocEntryBytes;
        store_u32_le(result.metadata.data() + entry, static_cast<std::uint32_t>(offset_units));
        store_u32_le(result.metadata.data() + entry + 4, static_cast<std::uint32_t>(start_units));
        store_u32_le(result.metadata.data() + entry + 8, static_cast<std::uint32_t>(length_units));

        const std::uint64_t device_offset = data_start_sector * apa::kSectorSize;
        result.payload_extents.push_back({source_offset, device_offset, take, i});
        source_offset += take;
        remaining -= take;
    }

    if (remaining != 0 || result.payload_extents.empty()) {
        result.error = "Planned APA extents did not cover the complete HDL payload";
        result.payload_extents.clear();
        return result;
    }
    if (result.payload_extents.size() != allocation.extents.size()) {
        result.error = "HDL allocation contains an unused main/sub extent without a DEADFEED entry";
        result.payload_extents.clear();
        return result;
    }
    result.metadata[kPartCountOffset] =
        static_cast<std::byte>(result.payload_extents.size());

    result.ok = true;
    return result;
}

} // namespace ps2hdd::hdl
