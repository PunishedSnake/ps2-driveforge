#include "ps2hdd/apa_volume.hpp"

#include <limits>

namespace ps2hdd {

ApaVolume::ApaVolume(BlockDevice& device, const apa::Partition& partition)
    : device_(device), partition_(partition)
{
    // PFS BlockInfo.subpart uses one logical index space: 0 is the APA main
    // partition and 1..N are its recorded APA sub-partitions. Keep that mapping
    // here so PFS, GUI and future Dokany code never need physical APA LBAs.
    extents_.push_back({partition.start_lba, partition.length_sectors});
    for (const auto& sub : partition.sub_partitions) {
        extents_.push_back({sub.start, sub.length});
    }
}

bool ApaVolume::read_sectors(std::size_t sub, std::uint32_t sector, std::uint32_t count,
                             std::span<std::byte> out)
{
    if (sub >= extents_.size()) {
        return false;
    }

    const auto& e = extents_[sub];
    const std::uint64_t end_sector = static_cast<std::uint64_t>(sector) + count;
    if (end_sector > e.length_sectors) {
        return false;
    }

    const std::uint64_t byte_count = static_cast<std::uint64_t>(count) * apa::kSectorSize;
    if (byte_count != out.size()) {
        return false;
    }

    // `sector` is relative to the selected logical APA extent. This is the only
    // place that adds the physical start LBA; callers must not pre-translate it.
    // Real HDD layouts have already shown that sub-partitions need not be
    // physically adjacent to the main extent, so simple contiguous arithmetic
    // is not a safe substitute for this lookup.
    const std::uint64_t physical_lba = static_cast<std::uint64_t>(e.start_lba) + sector;
    if (physical_lba > std::numeric_limits<std::uint64_t>::max() / apa::kSectorSize) {
        return false;
    }

    return device_.read(physical_lba * apa::kSectorSize, out);
}

} // namespace ps2hdd
