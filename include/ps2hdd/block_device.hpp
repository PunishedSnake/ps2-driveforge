#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ps2hdd {

class BlockDevice {
public:
    virtual ~BlockDevice() = default;

    [[nodiscard]] virtual std::uint64_t size_bytes() const = 0;
    [[nodiscard]] virtual std::string display_name() const = 0;

    // Exact read. Returns false when the complete requested range could not be read.
    virtual bool read(std::uint64_t offset, std::span<std::byte> out) = 0;
};

} // namespace ps2hdd
