#include "ps2hdd/apa.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace {

class FuzzDevice final : public ps2hdd::BlockDevice {
public:
    explicit FuzzDevice(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::uint64_t size_bytes() const override { return bytes_.size(); }
    [[nodiscard]] std::string display_name() const override { return "libfuzzer"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(out.data(), bytes_.data() + static_cast<std::ptrdiff_t>(offset), out.size());
        return true;
    }

private:
    std::span<const std::byte> bytes_;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < ps2hdd::apa::kHeaderSize) {
        return 0;
    }
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(data), size);
    FuzzDevice device(bytes);
    ps2hdd::apa::Reader reader(device);
    // The explicit limit is part of the fuzz contract: arbitrary metadata must
    // never turn a parser run into an unbounded linked-list walk.
    (void)reader.scan(256);
    return 0;
}
