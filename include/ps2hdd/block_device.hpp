#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ps2hdd {

enum class StorageMediaClass {
    unknown,
    rotational,
    solid_state,
};

struct StorageCharacteristics {
    StorageMediaClass media_class{StorageMediaClass::unknown};
    bool seek_penalty_known{};
    bool incurs_seek_penalty{};
    bool trim_known{};
    bool trim_enabled{};
    bool bus_type_known{};
    std::uint32_t bus_type{};
};

[[nodiscard]] inline const char* storage_media_class_name(StorageMediaClass value) noexcept
{
    switch (value) {
    case StorageMediaClass::rotational:
        return "rotational";
    case StorageMediaClass::solid_state:
        return "solid-state";
    case StorageMediaClass::unknown:
    default:
        return "unknown";
    }
}

class BlockDevice {
public:
    virtual ~BlockDevice() = default;

    [[nodiscard]] virtual std::uint64_t size_bytes() const = 0;
    [[nodiscard]] virtual std::string display_name() const = 0;

    // Optional host-storage hints. These are capability/characteristic signals,
    // not tuning commands: callers must remain correct when every field is unknown.
    // Runtime latency/pattern measurements may override performance assumptions.
    [[nodiscard]] virtual StorageCharacteristics storage_characteristics() const noexcept
    {
        return {};
    }

    // Exact read. Returns false when the complete requested range could not be read.
    virtual bool read(std::uint64_t offset, std::span<std::byte> out) = 0;
};

} // namespace ps2hdd
