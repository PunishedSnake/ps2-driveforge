#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2hdd::magicgate::cipher {

using Block = std::array<std::byte, 8>;
using DoubleKey = std::array<std::byte, 16>;

namespace detail {

inline constexpr std::array<unsigned, 64> kIp{
    58,50,42,34,26,18,10,2, 60,52,44,36,28,20,12,4,
    62,54,46,38,30,22,14,6, 64,56,48,40,32,24,16,8,
    57,49,41,33,25,17,9,1, 59,51,43,35,27,19,11,3,
    61,53,45,37,29,21,13,5, 63,55,47,39,31,23,15,7};
inline constexpr std::array<unsigned, 64> kFp{
    40,8,48,16,56,24,64,32, 39,7,47,15,55,23,63,31,
    38,6,46,14,54,22,62,30, 37,5,45,13,53,21,61,29,
    36,4,44,12,52,20,60,28, 35,3,43,11,51,19,59,27,
    34,2,42,10,50,18,58,26, 33,1,41,9,49,17,57,25};
inline constexpr std::array<unsigned, 56> kPc1{
    57,49,41,33,25,17,9, 1,58,50,42,34,26,18,
    10,2,59,51,43,35,27, 19,11,3,60,52,44,36,
    63,55,47,39,31,23,15, 7,62,54,46,38,30,22,
    14,6,61,53,45,37,29, 21,13,5,28,20,12,4};
inline constexpr std::array<unsigned, 48> kPc2{
    14,17,11,24,1,5, 3,28,15,6,21,10,
    23,19,12,4,26,8, 16,7,27,20,13,2,
    41,52,31,37,47,55, 30,40,51,45,33,48,
    44,49,39,56,34,53, 46,42,50,36,29,32};
inline constexpr std::array<unsigned, 48> kExpansion{
    32,1,2,3,4,5, 4,5,6,7,8,9, 8,9,10,11,12,13,
    12,13,14,15,16,17, 16,17,18,19,20,21,
    20,21,22,23,24,25, 24,25,26,27,28,29,
    28,29,30,31,32,1};
inline constexpr std::array<unsigned, 32> kPermutation{
    16,7,20,21, 29,12,28,17, 1,15,23,26, 5,18,31,10,
    2,8,24,14, 32,27,3,9, 19,13,30,6, 22,11,4,25};
inline constexpr std::array<unsigned, 16> kShifts{
    1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};

inline constexpr std::array<std::array<unsigned char, 64>, 8> kSBoxes{{
    {{14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,
      0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
      4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,
      15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13}},
    {{15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,
      3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
      0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,
      13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9}},
    {{10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,
      13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
      13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,
      1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12}},
    {{7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,
      13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
      10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,
      3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14}},
    {{2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,
      14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
      4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,
      11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3}},
    {{12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,
      10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
      9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,
      4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13}},
    {{4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,
      13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
      1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,
      6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12}},
    {{13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,
      1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
      7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,
      2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}}
}};

[[nodiscard]] constexpr std::uint64_t load_le64(const Block& bytes) noexcept
{
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[i])) << (i * 8U);
    }
    return value;
}

[[nodiscard]] constexpr Block store_le64(std::uint64_t value) noexcept
{
    Block out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
    }
    return out;
}

template <std::size_t N>
[[nodiscard]] constexpr std::uint64_t permute(std::uint64_t input,
                                               const std::array<unsigned, N>& table,
                                               unsigned input_bits) noexcept
{
    std::uint64_t output = 0;
    for (const auto source_position : table) {
        output <<= 1U;
        output |= (input >> (input_bits - source_position)) & 1U;
    }
    return output;
}

[[nodiscard]] constexpr std::uint32_t rotl28(std::uint32_t value, unsigned shift) noexcept
{
    value &= 0x0fffffffU;
    return ((value << shift) | (value >> (28U - shift))) & 0x0fffffffU;
}

using Schedule = std::array<std::uint64_t, 16>;

[[nodiscard]] constexpr Schedule make_schedule(std::uint64_t key) noexcept
{
    Schedule schedule{};
    const auto selected = permute(key, kPc1, 64);
    auto c = static_cast<std::uint32_t>((selected >> 28U) & 0x0fffffffU);
    auto d = static_cast<std::uint32_t>(selected & 0x0fffffffU);
    for (std::size_t round = 0; round < schedule.size(); ++round) {
        c = rotl28(c, kShifts[round]);
        d = rotl28(d, kShifts[round]);
        const auto cd = (static_cast<std::uint64_t>(c) << 28U) | d;
        schedule[round] = permute(cd, kPc2, 56);
    }
    return schedule;
}

[[nodiscard]] constexpr std::uint32_t round_function(std::uint32_t right,
                                                      std::uint64_t round_key) noexcept
{
    const auto expanded = permute(static_cast<std::uint64_t>(right), kExpansion, 32) ^ round_key;
    std::uint32_t substituted = 0;
    for (std::size_t box = 0; box < 8; ++box) {
        const auto six = static_cast<unsigned>((expanded >> (42U - box * 6U)) & 0x3fU);
        const auto row = ((six & 0x20U) >> 4U) | (six & 0x01U);
        const auto column = (six >> 1U) & 0x0fU;
        substituted = (substituted << 4U) | kSBoxes[box][row * 16U + column];
    }
    return static_cast<std::uint32_t>(permute(substituted, kPermutation, 32));
}

[[nodiscard]] constexpr std::uint64_t crypt(std::uint64_t input,
                                             const Schedule& schedule,
                                             bool decrypt) noexcept
{
    const auto initial = permute(input, kIp, 64);
    auto left = static_cast<std::uint32_t>(initial >> 32U);
    auto right = static_cast<std::uint32_t>(initial & 0xffffffffU);
    for (std::size_t round = 0; round < 16; ++round) {
        const auto key_index = decrypt ? 15U - round : round;
        const auto next = left ^ round_function(right, schedule[key_index]);
        left = right;
        right = next;
    }
    const auto preoutput = (static_cast<std::uint64_t>(right) << 32U) | left;
    return permute(preoutput, kFp, 64);
}

[[nodiscard]] constexpr Block key_half(const DoubleKey& key, std::size_t offset) noexcept
{
    Block half{};
    for (std::size_t i = 0; i < half.size(); ++i) {
        half[i] = key[offset + i];
    }
    return half;
}

} // namespace detail

// MechaCon software references load DES key/data bytes as little-endian 64-bit
// values before applying the standard DES bit permutations. Keeping that byte
// convention here is essential: using a library API with the usual network-byte
// examples without adapting it produces perfectly valid DES and perfectly wrong
// MagicGate bytes.
[[nodiscard]] constexpr Block des_encrypt(const Block& data, const Block& key) noexcept
{
    const auto schedule = detail::make_schedule(detail::load_le64(key));
    return detail::store_le64(detail::crypt(detail::load_le64(data), schedule, false));
}

[[nodiscard]] constexpr Block des_decrypt(const Block& data, const Block& key) noexcept
{
    const auto schedule = detail::make_schedule(detail::load_le64(key));
    return detail::store_le64(detail::crypt(detail::load_le64(data), schedule, true));
}

[[nodiscard]] constexpr Block tdes2_encrypt(const Block& data, const DoubleKey& key) noexcept
{
    const auto k1 = detail::key_half(key, 0);
    const auto k2 = detail::key_half(key, 8);
    return des_encrypt(des_decrypt(des_encrypt(data, k1), k2), k1);
}

[[nodiscard]] constexpr Block tdes2_decrypt(const Block& data, const DoubleKey& key) noexcept
{
    const auto k1 = detail::key_half(key, 0);
    const auto k2 = detail::key_half(key, 8);
    return des_decrypt(des_encrypt(des_decrypt(data, k1), k2), k1);
}

[[nodiscard]] constexpr Block xor_block(const Block& a, const Block& b) noexcept
{
    Block out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = a[i] ^ b[i];
    }
    return out;
}

// One-block CBC is the only operation needed for the disk Kbit/Kc key schedule
// and key wrapping. It is kept explicit rather than pretending we already need
// a general streaming cipher API.
[[nodiscard]] constexpr Block tdes2_cbc_encrypt_block(const Block& data,
                                                       const DoubleKey& key,
                                                       const Block& iv) noexcept
{
    return tdes2_encrypt(xor_block(data, iv), key);
}

[[nodiscard]] constexpr Block tdes2_cbc_decrypt_block(const Block& data,
                                                       const DoubleKey& key,
                                                       const Block& iv) noexcept
{
    return xor_block(tdes2_decrypt(data, key), iv);
}

} // namespace ps2hdd::magicgate::cipher
