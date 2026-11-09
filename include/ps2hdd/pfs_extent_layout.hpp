#pragma once

#include "ps2hdd/pfs.hpp"

#include <span>
#include <string>
#include <vector>

namespace ps2hdd::pfs {

struct SegmentWrite {
    BlockInfo location{};
    SegmentDescriptor descriptor{};
};

struct FileExtentLayout {
    bool ok{};
    std::string error;
    Inode inode{};
    std::vector<SegmentWrite> segments;
};

// Number of SEGI metadata blocks required to represent `data_extent_count`
// logical data descriptors after the 113 direct slots in the base SEGD inode.
[[nodiscard]] std::size_t required_segi_count(std::size_t data_extent_count) noexcept;

// Pure serializer for a file's extent graph. `base` supplies inode identity,
// mode/timestamps/size and other non-layout metadata. The function rewrites only
// block-layout fields/checksum, fills chained SEGI descriptors and performs no I/O.
//
// `segi_locations` must contain exactly required_segi_count(data_extents.size())
// one-zone metadata locations. SEGI metadata zones do not contribute to
// inode.number_blocks because they are filesystem metadata, not logical payload.
[[nodiscard]] FileExtentLayout build_file_extent_layout(
    const Inode& base,
    std::span<const BlockInfo> data_extents,
    std::span<const BlockInfo> segi_locations);

} // namespace ps2hdd::pfs
