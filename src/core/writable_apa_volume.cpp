#include "ps2hdd/writable_apa_volume.hpp"

#include <cstdint>
#include <limits>

namespace ps2hdd {

WritableApaVolume::WritableApaVolume(WritableBlockDevice& device,
                                     const apa::Partition& partition)
    : device_(device), partition_(partition)
{
    extents_.push_back({partition.start_lba, partition.length_sectors});
    extents_.reserve(1 + partition.sub_partitions.size());
    for (const auto& sub : partition.sub_partitions) {
        extents_.push_back({sub.start, sub.length});
    }
}

bool WritableApaVolume::byte_range(std::size_t sub, std::uint32_t sector,
                                   std::uint32_t count, std::size_t bytes,
                                   std::uint64_t& offset) const noexcept
{
    if (sub >= extents_.size()) {
        return false;
    }
    const auto& extent = extents_[sub];
    if (sector > extent.length_sectors || count > extent.length_sectors - sector) {
        return false;
    }

    const std::uint64_t expected = static_cast<std::uint64_t>(count) * apa::kSectorSize;
    if (expected != bytes) {
        return false;
    }

    const std::uint64_t absolute_sector = static_cast<std::uint64_t>(extent.start_lba) + sector;
    if (absolute_sector > std::numeric_limits<std::uint64_t>::max() / apa::kSectorSize) {
        return false;
    }
    offset = absolute_sector * apa::kSectorSize;
    if (offset > device_.size_bytes() || expected > device_.size_bytes() - offset) {
        return false;
    }
    return true;
}

bool WritableApaVolume::read_sectors(std::size_t sub, std::uint32_t sector,
                                     std::uint32_t count, std::span<std::byte> out)
{
    std::uint64_t offset = 0;
    if (!byte_range(sub, sector, count, out.size(), offset)) {
        return false;
    }
    return device_.read(offset, out);
}

bool WritableApaVolume::write_sectors(std::size_t sub, std::uint32_t sector,
                                      std::uint32_t count, std::span<const std::byte> in)
{
    std::uint64_t offset = 0;
    if (!byte_range(sub, sector, count, in.size(), offset)) {
        return false;
    }
    return device_.write(offset, in);
}

} // namespace ps2hdd
