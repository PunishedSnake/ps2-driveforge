#include "ps2hdd/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace ps2hdd::crypto {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept
{
    return (value >> bits) | (value << (32U - bits));
}

[[nodiscard]] std::uint32_t read_be32(const std::byte* p) noexcept
{
    return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3]));
}

void write_be32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>((value >> 24U) & 0xffU);
    p[1] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[3] = static_cast<std::byte>(value & 0xffU);
}

void transform(Sha256Context& context, const std::byte* block) noexcept
{
    std::array<std::uint32_t, 16> schedule{};
    auto a = context.state[0];
    auto b = context.state[1];
    auto c = context.state[2];
    auto d = context.state[3];
    auto e = context.state[4];
    auto f = context.state[5];
    auto g = context.state[6];
    auto h = context.state[7];

    for (unsigned i = 0; i < 64; ++i) {
        std::uint32_t word = 0;
        if (i < 16) {
            word = read_be32(block + i * 4U);
            schedule[i] = word;
        } else {
            const auto w15 = schedule[(i - 15U) & 15U];
            const auto w2 = schedule[(i - 2U) & 15U];
            const auto s0 = rotr(w15, 7) ^ rotr(w15, 18) ^ (w15 >> 3U);
            const auto s1 = rotr(w2, 17) ^ rotr(w2, 19) ^ (w2 >> 10U);
            word = schedule[i & 15U] + s0 + schedule[(i - 7U) & 15U] + s1;
            schedule[i & 15U] = word;
        }

        const auto sum1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const auto choose = (e & f) ^ ((~e) & g);
        const auto temporary1 = h + sum1 + choose + kRoundConstants[i] + word;
        const auto sum0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
    context.state[4] += e;
    context.state[5] += f;
    context.state[6] += g;
    context.state[7] += h;
}

} // namespace

void sha256_init(Sha256Context& context) noexcept
{
    context.state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                     0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    context.total_bytes = 0;
    context.block.fill(std::byte{0});
    context.block_used = 0;
}

void sha256_update(Sha256Context& context, std::span<const std::byte> data) noexcept
{
    context.total_bytes += data.size();
    std::size_t cursor = 0;

    if (context.block_used != 0 && !data.empty()) {
        const auto available = context.block.size() - context.block_used;
        const auto amount = std::min(available, data.size());
        std::copy_n(data.begin(), amount,
                    context.block.begin() + static_cast<std::ptrdiff_t>(context.block_used));
        context.block_used += amount;
        cursor += amount;
        if (context.block_used == context.block.size()) {
            transform(context, context.block.data());
            context.block_used = 0;
        }
    }

    while (context.block_used == 0 && data.size() - cursor >= context.block.size()) {
        transform(context, data.data() + static_cast<std::ptrdiff_t>(cursor));
        cursor += context.block.size();
    }

    if (cursor < data.size()) {
        const auto amount = data.size() - cursor;
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(cursor), amount,
                    context.block.begin());
        context.block_used = amount;
    }
}

Sha256Digest sha256_final(Sha256Context& context) noexcept
{
    const auto total_bits = context.total_bytes * 8ULL;
    context.block[context.block_used++] = std::byte{0x80};
    if (context.block_used > 56) {
        std::fill(context.block.begin() + static_cast<std::ptrdiff_t>(context.block_used),
                  context.block.end(), std::byte{0});
        transform(context, context.block.data());
        context.block_used = 0;
    }
    std::fill(context.block.begin() + static_cast<std::ptrdiff_t>(context.block_used),
              context.block.begin() + 56, std::byte{0});
    for (unsigned i = 0; i < 8; ++i) {
        context.block[63U - i] = static_cast<std::byte>((total_bits >> (i * 8U)) & 0xffULL);
    }
    transform(context, context.block.data());

    Sha256Digest digest{};
    for (unsigned i = 0; i < 8; ++i) {
        write_be32(digest.data() + i * 4U, context.state[i]);
    }
    context = {};
    return digest;
}

Sha256Digest sha256(std::span<const std::byte> data) noexcept
{
    Sha256Context context;
    sha256_init(context);
    sha256_update(context, data);
    return sha256_final(context);
}

std::string sha256_hex(const Sha256Digest& digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const auto value = std::to_integer<unsigned char>(digest[i]);
        out[i * 2] = digits[value >> 4U];
        out[i * 2 + 1] = digits[value & 0x0fU];
    }
    return out;
}

} // namespace ps2hdd::crypto
