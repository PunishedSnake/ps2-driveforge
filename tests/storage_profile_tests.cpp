#include "ps2hdd/instrumented_block_device.hpp"
#include "ps2hdd/read_ahead_block_device.hpp"
#include "ps2hdd/read_cache_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeDevice final : public ps2hdd::BlockDevice {
public:
    [[nodiscard]] std::uint64_t size_bytes() const override { return 1024 * 1024; }
    [[nodiscard]] std::string display_name() const override { return "fake"; }
    [[nodiscard]] ps2hdd::StorageCharacteristics storage_characteristics() const noexcept override
    {
        ps2hdd::StorageCharacteristics value;
        value.media_class = ps2hdd::StorageMediaClass::rotational;
        value.seek_penalty_known = true;
        value.incurs_seek_penalty = true;
        value.trim_known = true;
        value.trim_enabled = false;
        value.bus_type_known = true;
        value.bus_type = 7;
        return value;
    }
    bool read(std::uint64_t, std::span<std::byte> out) override
    {
        for (auto& value : out) {
            value = std::byte{0};
        }
        return true;
    }
};

void storage_profile_roundtrip()
{
    FakeDevice source;
    ps2hdd::InstrumentedBlockDevice instrumented(source);
    ps2hdd::ReadAheadBlockDevice ahead(instrumented);
    ps2hdd::ReadCacheBlockDevice cache(ahead);

    const auto profile = cache.storage_characteristics();
    check(profile.media_class == ps2hdd::StorageMediaClass::rotational,
          "media class propagates through all wrappers");
    check(profile.seek_penalty_known && profile.incurs_seek_penalty,
          "seek penalty propagates through all wrappers");
    check(profile.trim_known && !profile.trim_enabled,
          "TRIM characteristic propagates through all wrappers");
    check(profile.bus_type_known && profile.bus_type == 7,
          "bus type propagates through all wrappers");
    check(std::string(ps2hdd::storage_media_class_name(ps2hdd::StorageMediaClass::unknown)) == "unknown",
          "unknown media class has stable display name");
    check(std::string(ps2hdd::storage_media_class_name(ps2hdd::StorageMediaClass::solid_state)) == "solid-state",
          "solid-state media class has stable display name");
}

} // namespace

int main()
{
    try {
        storage_profile_roundtrip();
        std::cout << "Storage profile tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
