#include "ps2hdd/read_cache_block_device.hpp"

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
    explicit CountingDevice(std::size_t bytes) : bytes_(bytes)
    {
        for (std::size_t i = 0; i < bytes_.size(); ++i) {
            bytes_[i] = static_cast<std::byte>(i & 0xFFU);
        }
    }

    [[nodiscard]] std::uint64_t size_bytes() const override { return bytes_.size(); }
    [[nodiscard]] std::string display_name() const override { return "read-cache fixture"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        ++calls;
        last_size = out.size();
        std::memcpy(out.data(), bytes_.data() + static_cast<std::ptrdiff_t>(offset), out.size());
        return true;
    }

    std::uint64_t calls{};
    std::size_t last_size{};

private:
    std::vector<std::byte> bytes_;
};

void cache_contract()
{
    CountingDevice backing(32U * 1024U);
    ps2hdd::ReadCacheBlockDevice cache(backing);

    std::array<std::byte, 5> first{};
    check(cache.read(123, first), "first small read succeeds");
    check(backing.calls == 1 && backing.last_size == ps2hdd::ReadCacheBlockDevice::kPageSize,
          "first small read fills exactly one 4 KiB page");
    check(first[0] == static_cast<std::byte>(123), "first payload byte is correct");

    cache.reset_stats();
    std::array<std::byte, 5> second{};
    check(cache.read(123, second), "repeat small read succeeds");
    check(backing.calls == 1, "repeat small read performs zero extra backing reads");
    const auto warm = cache.stats();
    check(warm.hits == 1 && warm.misses == 0 && warm.bytes_served == second.size(),
          "repeat small read is counted as a cache hit");

    // Large existing PFS batches must remain one direct backend request rather
    // than being fragmented into cache-page reads.
    cache.reset_stats();
    std::array<std::byte, 8192> large{};
    check(cache.read(8192, large), "large read succeeds");
    check(backing.calls == 2 && backing.last_size == large.size(),
          "large read bypasses cache as one backing request");
    check(cache.stats().bypass_reads == 1, "large bypass is counted");

    // A request crossing a page boundary also bypasses so the cache never turns
    // one caller request into two backend I/Os.
    cache.reset_stats();
    std::array<std::byte, 4> crossing{};
    check(cache.read(4094, crossing), "cross-page read succeeds");
    check(backing.calls == 3 && backing.last_size == crossing.size(),
          "cross-page read remains one direct backing request");
    check(cache.stats().bypass_reads == 1, "cross-page bypass is counted");

    cache.clear();
    cache.reset_stats();
    std::array<std::byte, 5> after_clear{};
    check(cache.read(123, after_clear), "read after clear succeeds");
    check(backing.calls == 4, "cache clear restores a backing page fill");
    check(cache.stats().misses == 1 && cache.stats().fill_bytes == 4096,
          "cold page fill after clear is instrumented");

    std::array<std::byte, 8> invalid{};
    check(!cache.read(backing.size_bytes() - 4, invalid), "out-of-range read remains rejected");
}

} // namespace

int main()
{
    try {
        cache_contract();
        std::cout << "Emilia read-window cache tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
