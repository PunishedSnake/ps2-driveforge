#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ps2hdd {

// Capability-specific APA extent mapper for format writers. This is deliberately
// separate from the read-only ApaVolume used by all existing PFS readers.
class WritableApaVolume final {
public:
    struct Extent {
        std::uint32_t start_lba{};
        std::uint32_t length_sectors{};
    };

    WritableApaVolume(WritableBlockDevice& device, const apa::Partition& partition);

    [[nodiscard]] const apa::Partition& partition() const noexcept { return partition_; }
    [[nodiscard]] std::size_t extent_count() const noexcept { return extents_.size(); }
    [[nodiscard]] const Extent& extent(std::size_t sub) const { return extents_.at(sub); }

    bool read_sectors(std::size_t sub, std::uint32_t sector, std::uint32_t count,
                      std::span<std::byte> out);
    bool write_sectors(std::size_t sub, std::uint32_t sector, std::uint32_t count,
                       std::span<const std::byte> in);

    // Resolve a validated main/sub-relative sector range to the backing device
    // byte offset. Format-level transactions use this to stage metadata through
    // WriteTransaction without exposing or duplicating APA extent arithmetic.
    [[nodiscard]] bool absolute_byte_offset(std::size_t sub, std::uint32_t sector,
                                            std::uint32_t count,
                                            std::uint64_t& offset) const noexcept;

    bool flush() noexcept { return device_.flush(); }

private:
    [[nodiscard]] bool byte_range(std::size_t sub, std::uint32_t sector,
                                  std::uint32_t count, std::size_t bytes,
                                  std::uint64_t& offset) const noexcept;

    WritableBlockDevice& device_;
    apa::Partition partition_;
    std::vector<Extent> extents_;
};

} // namespace ps2hdd
