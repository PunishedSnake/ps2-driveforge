#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ps2hdd::crypto {

using Sha256Digest = std::array<std::byte, 32>;

struct Sha256Context {
    std::array<std::uint32_t, 8> state{};
    std::uint64_t total_bytes{};
    std::array<std::byte, 64> block{};
    std::size_t block_used{};
};

void sha256_init(Sha256Context& context) noexcept;
void sha256_update(Sha256Context& context, std::span<const std::byte> data) noexcept;
[[nodiscard]] Sha256Digest sha256_final(Sha256Context& context) noexcept;
[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);

} // namespace ps2hdd::crypto
