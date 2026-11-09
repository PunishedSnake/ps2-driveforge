#include "ps2hdd/pfs_extent_layout.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ps2hdd::pfs {

std::size_t required_segi_count(std::size_t data_extent_count) noexcept
{
    constexpr std::size_t direct_capacity = kInodeMaxBlocks - 1U;
    constexpr std::size_t indirect_capacity = kIndirectMaxBlocks - 1U;
    if (data_extent_count <= direct_capacity) {
        return 0;
    }
    const auto indirect = data_extent_count - direct_capacity;
    return (indirect + indirect_capacity - 1U) / indirect_capacity;
}

FileExtentLayout build_file_extent_layout(const Inode& base,
                                          std::span<const BlockInfo> data_extents,
                                          std::span<const BlockInfo> segi_locations)
{
    FileExtentLayout result;
    const auto needed_segi = required_segi_count(data_extents.size());
    if (segi_locations.size() != needed_segi) {
        result.error = "PFS SEGI location count does not match extent layout requirements";
        return result;
    }
    if (needed_segi > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "PFS SEGI descriptor count overflows inode metadata";
        return result;
    }

    std::uint64_t payload_zones = 0;
    for (const auto& extent : data_extents) {
        if (extent.count == 0) {
            result.error = "PFS file extent layout contains an empty data descriptor";
            return result;
        }
        if (payload_zones > std::numeric_limits<std::uint32_t>::max() - extent.count) {
            result.error = "PFS file extent zone count overflows inode metadata";
            return result;
        }
        payload_zones += extent.count;
    }
    for (const auto& location : segi_locations) {
        if (location.count != 1 || location.number == 0) {
            result.error = "PFS SEGI metadata location must describe exactly one non-zero zone";
            return result;
        }
    }

    result.inode = base;
    result.inode.magic = kSegdMagic;
    result.inode.next_segment = {};
    result.inode.last_segment = result.inode.inode_block;
    for (auto& block : result.inode.data) {
        block = {};
    }
    result.inode.data[0] = result.inode.inode_block;

    constexpr std::size_t direct_capacity = kInodeMaxBlocks - 1U;
    const auto direct_count = std::min(data_extents.size(), direct_capacity);
    for (std::size_t index = 0; index < direct_count; ++index) {
        result.inode.data[index + 1U] = data_extents[index];
    }

    result.segments.reserve(needed_segi);
    std::size_t data_cursor = direct_count;
    BlockInfo previous = result.inode.inode_block;
    for (std::size_t segi_index = 0; segi_index < needed_segi; ++segi_index) {
        SegmentWrite write;
        write.location = segi_locations[segi_index];
        auto& descriptor = write.descriptor;
        descriptor.magic = kSegiMagic;
        descriptor.inode_block = result.inode.inode_block;
        descriptor.last_segment = previous;
        descriptor.next_segment = segi_index + 1U < needed_segi
                                      ? segi_locations[segi_index + 1U]
                                      : BlockInfo{};
        descriptor.data[0] = write.location;

        constexpr std::size_t indirect_capacity = kIndirectMaxBlocks - 1U;
        const auto take = std::min(indirect_capacity, data_extents.size() - data_cursor);
        for (std::size_t local = 0; local < take; ++local) {
            descriptor.data[local + 1U] = data_extents[data_cursor + local];
        }
        data_cursor += take;
        descriptor.checksum = segment_checksum(descriptor);
        previous = write.location;
        result.segments.push_back(write);
    }

    if (!result.segments.empty()) {
        result.inode.next_segment = result.segments.front().location;
        result.inode.last_segment = result.segments.back().location;
    }

    const auto number_data64 = 1ULL + data_extents.size() + result.segments.size();
    if (number_data64 > std::numeric_limits<std::uint32_t>::max() ||
        payload_zones > std::numeric_limits<std::uint32_t>::max() - 1U) {
        result.error = "PFS extent layout counters overflow inode metadata";
        return result;
    }
    result.inode.number_data = static_cast<std::uint32_t>(number_data64);
    result.inode.number_blocks = static_cast<std::uint32_t>(1U + payload_zones);
    // Existing DriveForge readers/writers use 1 for the base-only direct layout.
    // With indirect metadata present, store the traversable SEGI count while
    // preserving that established direct-layout convention.
    result.inode.number_segdesg = static_cast<std::uint32_t>(
        std::max<std::size_t>(1U, result.segments.size()));
    result.inode.subpart = result.inode.inode_block.subpart;
    result.inode.checksum = inode_checksum(result.inode);

    if (data_cursor != data_extents.size()) {
        result.error = "PFS SEGI serializer did not consume every data extent";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace ps2hdd::pfs
