#pragma once

#include "ps2hdd/block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ps2hdd {

// Writable access is an explicit capability rather than a member of BlockDevice.
// Read-only sessions therefore cannot accidentally gain mutation just because a
// backend happens to support it. Writable images and the separately gated
// WritablePhysicalDrive both implement this contract; ordinary PhysicalDrive
// stays read-only. Same bytes, very different amount of regret if admission is
// wrong, so callers still have to earn the physical capability explicitly.
class WritableBlockDevice : public BlockDevice {
public:
    ~WritableBlockDevice() override = default;

    // Exact in-place write. Implementations must reject requests that extend the
    // existing backing store; resizing/partition allocation is format-layer work.
    virtual bool write(std::uint64_t offset, std::span<const std::byte> in) = 0;

    // Make previously completed writes durable before reporting success to the
    // format layer. This is intentionally explicit so higher-level transactions
    // can choose their commit point.
    virtual bool flush() = 0;
};

} // namespace ps2hdd
