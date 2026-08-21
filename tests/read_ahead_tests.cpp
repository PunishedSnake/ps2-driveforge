#include "ps2hdd/read_ahead_block_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class CountingDevice final : public ps2hdd::BlockDevice {
public:
    CountingDevice() : bytes_(1024U * 1024U)
    {
        for (std::size_t i = 0; i < bytes_.size(); ++i) {
            bytes_[i] = static_cast<std::byte>(i & 0xFFU);
        }
    }

    [[nodiscard]] std::uint64_t size_bytes() const override { return bytes_.size(); }
    [[nodiscard]] std::string display_name() const override { return "read-ahead fixture"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        ++calls;
        bytes_read += out.size();
        std::memcpy(out.data(), bytes_.data() + static_cast<std::size_t>(offset), out.size());
        return true;
    }

    std::uint64_t calls{};
    std::uint64_t bytes_read{};

private:
    std::vector<std::byte> bytes_;
};

void sequential_prefetch_roundtrip()
{
    CountingDevice backing;
    ps2hdd::ReadAheadBlockDevice ahead(backing);

    std::array<std::byte, 64U * 1024U> block{};
    check(ahead.read(0, block), "first sequential block reads");
    check(backing.calls == 1 && backing.bytes_read == block.size(),
          "first large read does not speculate");

    check(ahead.read(block.size(), block), "second sequential block reads");
    check(backing.calls == 2, "second sequential block uses one combined backend read");
    check(backing.bytes_read == block.size() + block.size() + ps2hdd::ReadAheadBlockDevice::kLookAheadBytes,
          "second sequential block adds configured lookahead");

    const auto calls_after_prefetch = backing.calls;
    check(ahead.read(block.size() * 2ULL, block), "third sequential block reads from window");
    check(backing.calls == calls_after_prefetch, "third sequential block performs zero backend reads");

    const auto stats = ahead.stats();
    check(stats.prefetches == 1, "one prefetch is recorded");
    check(stats.prefetched_bytes == ps2hdd::ReadAheadBlockDevice::kLookAheadBytes,
          "prefetched byte count excludes requested payload");
    check(stats.window_hits >= 1 && stats.bytes_served_from_window >= block.size(),
          "subsequent sequential payload is served from read-ahead window");

    std::array<std::byte, 4096> small{};
    const auto before_small = backing.calls;
    check(ahead.read(900U * 1024U, small), "random small read succeeds");
    check(backing.calls == before_small + 1, "random small read bypasses speculative lookahead");

    ahead.clear();
    const auto before_clear_read = backing.calls;
    check(ahead.read(0, block), "read succeeds after explicit read-ahead clear");
    check(backing.calls == before_clear_read + 1, "clear restores cold sequential path");
}

} // namespace

int main()
{
    try {
        sequential_prefetch_roundtrip();
        std::cout << "Emilia adaptive read-ahead tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
