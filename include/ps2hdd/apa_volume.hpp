#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ps2hdd {

class ApaVolume {
public:
    struct Extent {
        std::uint32_t start_lba{};
        std::uint32_t length_sectors{};
    };

    ApaVolume(BlockDevice& device, const apa::Partition& partition);

    [[nodiscard]] const apa::Partition& partition() const noexcept { return partition_; }
    [[nodiscard]] std::size_t extent_count() const noexcept { return extents_.size(); }
    [[nodiscard]] const Extent& extent(std::size_t sub) const { return extents_.at(sub); }

    // PFS-style addressing: sub=0 is the main APA partition, sub=1+ are APA subs.
    bool read_sectors(std::size_t sub, std::uint32_t sector, std::uint32_t count,
                      std::span<std::byte> out);

private:
    BlockDevice& device_;
    apa::Partition partition_;
    std::vector<Extent> extents_;
};

} // namespace ps2hdd
