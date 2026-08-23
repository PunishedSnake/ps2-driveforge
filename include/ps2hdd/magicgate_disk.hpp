#pragma once

#include "ps2hdd/magicgate_cipher.hpp"
#include "ps2hdd/magicgate_kelf.hpp"
#include "ps2hdd/magicgate_known_vectors.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace ps2hdd::magicgate {

struct DiskKeyset {
    // DriveForge deliberately ships no Sony key material. These values are an
    // injected capability, not constants hidden in the executable. Keeping the
    // two master keys and their derivation IV/material values separate mirrors
    // the disk-KELF path and avoids dragging memory-card authentication keys
    // into a feature that does not need them.
    cipher::Block kbit_material{};
    cipher::Block kc_material{};
    cipher::DoubleKey kbit_master{};
    cipher::DoubleKey kc_master{};
};

struct DiskContentKeys {
    std::array<std::byte, 16> kbit{};
    std::array<std::byte, 16> kc{};
};

struct DiskKeyResult {
    bool ok{};
    std::string error;
    DiskContentKeys keys;
    std::size_t content_key_offset{};
};

namespace detail {

[[nodiscard]] constexpr cipher::Block user_header_xor(
    const std::array<std::byte, 16>& header) noexcept
{
    cipher::Block out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = header[i] ^ header[i + 8U];
    }
    return out;
}

[[nodiscard]] constexpr cipher::DoubleKey derive_file_key(
    const KelfHeader& header, const DiskKeyset& keyset) noexcept
{
    const auto input = user_header_xor(header.user_header);
    // KELFTool expresses the same operation as (HeaderData XOR IV) followed by
    // 2-key TDES with a zero CBC IV. Passing HeaderData with the material as the
    // one-block CBC IV is algebraically identical and makes the role explicit.
    const auto kbit = cipher::tdes2_cbc_encrypt_block(
        input, keyset.kbit_master, keyset.kbit_material);
    const auto kc = cipher::tdes2_cbc_encrypt_block(
        input, keyset.kc_master, keyset.kc_material);

    cipher::DoubleKey file_key{};
    std::copy(kbit.begin(), kbit.end(), file_key.begin());
    std::copy(kc.begin(), kc.end(), file_key.begin() + 8);
    return file_key;
}

[[nodiscard]] constexpr cipher::Block load_block(std::span<const std::byte> bytes,
                                                  std::size_t offset) noexcept
{
    cipher::Block out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = bytes[offset + i];
    }
    return out;
}

constexpr void store_key_half(std::array<std::byte, 16>& key,
                              std::size_t offset,
                              const cipher::Block& block) noexcept
{
    for (std::size_t i = 0; i < block.size(); ++i) {
        key[offset + i] = block[i];
    }
}

[[nodiscard]] constexpr cipher::Block key_half(
    const std::array<std::byte, 16>& key,
    std::size_t offset) noexcept
{
    cipher::Block out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = key[offset + i];
    }
    return out;
}

} // namespace detail

// Convert the 32-byte disk representation E(file-key, Kbit || Kc) into the
// plaintext Kbit/Kc pair. This is the host-only portion that PC binding tools
// perform before entering the separate memory-card session/storage-key protocol.
// No card, USB adapter or MechaCon state belongs in this function.
[[nodiscard]] inline DiskKeyResult unwrap_disk_content_keys(
    std::span<const std::byte> file,
    const DiskKeyset& keyset)
{
    DiskKeyResult result;
    const auto layout = inspect_kelf(file);
    if (!layout.ok) {
        result.error = layout.error;
        return result;
    }
    const auto encrypted = content_key_bytes(file, layout);
    if (encrypted.size() != kKelfContentKeyBytes) {
        result.error = "KELF content-key window is unavailable";
        return result;
    }

    const auto file_key = detail::derive_file_key(layout.header, keyset);
    constexpr cipher::Block zero_iv{};
    detail::store_key_half(result.keys.kbit, 0,
        cipher::tdes2_cbc_decrypt_block(detail::load_block(encrypted, 0), file_key, zero_iv));
    detail::store_key_half(result.keys.kbit, 8,
        cipher::tdes2_cbc_decrypt_block(detail::load_block(encrypted, 8), file_key, zero_iv));
    detail::store_key_half(result.keys.kc, 0,
        cipher::tdes2_cbc_decrypt_block(detail::load_block(encrypted, 16), file_key, zero_iv));
    detail::store_key_half(result.keys.kc, 8,
        cipher::tdes2_cbc_decrypt_block(detail::load_block(encrypted, 24), file_key, zero_iv));

    result.content_key_offset = layout.content_key_offset;
    result.ok = true;
    return result;
}

// Produce the exact 32 bytes expected in a disk KELF header. Each 8-byte half
// starts with a zero CBC IV. Do not accidentally chain the four halves together:
// that would be a reasonable CBC design and the wrong MagicGate format.
[[nodiscard]] inline std::array<std::byte, kKelfContentKeyBytes>
wrap_disk_content_keys(const KelfLayout& layout,
                       const DiskContentKeys& keys,
                       const DiskKeyset& keyset)
{
    std::array<std::byte, kKelfContentKeyBytes> out{};
    if (!layout.ok) {
        return out;
    }
    const auto file_key = detail::derive_file_key(layout.header, keyset);
    constexpr cipher::Block zero_iv{};

    const auto kbit0 = cipher::tdes2_cbc_encrypt_block(
        detail::key_half(keys.kbit, 0), file_key, zero_iv);
    const auto kbit1 = cipher::tdes2_cbc_encrypt_block(
        detail::key_half(keys.kbit, 8), file_key, zero_iv);
    const auto kc0 = cipher::tdes2_cbc_encrypt_block(
        detail::key_half(keys.kc, 0), file_key, zero_iv);
    const auto kc1 = cipher::tdes2_cbc_encrypt_block(
        detail::key_half(keys.kc, 8), file_key, zero_iv);

    std::copy(kbit0.begin(), kbit0.end(), out.begin());
    std::copy(kbit1.begin(), kbit1.end(), out.begin() + 8);
    std::copy(kc0.begin(), kc0.end(), out.begin() + 16);
    std::copy(kc1.begin(), kc1.end(), out.begin() + 24);
    return out;
}

[[nodiscard]] inline bool write_wrapped_disk_content_keys(
    std::span<std::byte> file,
    const KelfLayout& layout,
    const DiskContentKeys& keys,
    const DiskKeyset& keyset) noexcept
{
    if (!layout.ok || layout.content_key_offset > file.size() ||
        kKelfContentKeyBytes > file.size() - layout.content_key_offset) {
        return false;
    }
    const auto wrapped = wrap_disk_content_keys(layout, keys, keyset);
    std::copy(wrapped.begin(), wrapped.end(),
              file.begin() + static_cast<std::ptrdiff_t>(layout.content_key_offset));
    return true;
}

namespace selftest {

constexpr cipher::Block kDesKey{
    std::byte{0x13}, std::byte{0x34}, std::byte{0x57}, std::byte{0x79},
    std::byte{0x9b}, std::byte{0xbc}, std::byte{0xdf}, std::byte{0xf1}};
constexpr cipher::Block kDesPlaintext{
    std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
    std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef}};
constexpr cipher::Block kDesCiphertext{
    std::byte{0x85}, std::byte{0xe8}, std::byte{0x13}, std::byte{0x54},
    std::byte{0x0f}, std::byte{0x0a}, std::byte{0xb4}, std::byte{0x05}};

// Classic FIPS DES known-answer vector. This catches both algorithm mistakes
// and representation mistakes before any user-supplied MagicGate keyset is used.
static_assert(cipher::des_encrypt(kDesPlaintext, kDesKey) == kDesCiphertext);
static_assert(cipher::des_decrypt(kDesCiphertext, kDesKey) == kDesPlaintext);

constexpr cipher::DoubleKey kSyntheticDoubleKey{
    std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
    std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef},
    std::byte{0xfe}, std::byte{0xdc}, std::byte{0xba}, std::byte{0x98},
    std::byte{0x76}, std::byte{0x54}, std::byte{0x32}, std::byte{0x10}};
static_assert(cipher::tdes2_decrypt(
                  cipher::tdes2_encrypt(kDesPlaintext, kSyntheticDoubleKey),
                  kSyntheticDoubleKey) == kDesPlaintext);

} // namespace selftest

} // namespace ps2hdd::magicgate