#pragma once

#include "ps2hdd/magicgate_cipher.hpp"

#include <cstddef>
#include <span>

namespace ps2hdd::magicgate::cbc {

namespace detail {

[[nodiscard]] constexpr cipher::Block load_block(std::span<const std::byte> bytes,
                                                  std::size_t offset) noexcept
{
    cipher::Block block{};
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = bytes[offset + i];
    }
    return block;
}

constexpr void store_block(std::span<std::byte> bytes,
                           std::size_t offset,
                           const cipher::Block& block) noexcept
{
    for (std::size_t i = 0; i < block.size(); ++i) {
        bytes[offset + i] = block[i];
    }
}

template <typename EncryptBlock>
[[nodiscard]] constexpr bool encrypt_in_place(std::span<std::byte> bytes,
                                               cipher::Block iv,
                                               EncryptBlock&& encrypt_block) noexcept
{
    if ((bytes.size() % cipher::Block{}.size()) != 0) {
        return false;
    }

    for (std::size_t offset = 0; offset < bytes.size(); offset += 8U) {
        const auto plaintext = load_block(bytes, offset);
        const auto ciphertext = encrypt_block(cipher::xor_block(plaintext, iv));
        store_block(bytes, offset, ciphertext);
        iv = ciphertext;
    }
    return true;
}

template <typename DecryptBlock>
[[nodiscard]] constexpr bool decrypt_in_place(std::span<std::byte> bytes,
                                               cipher::Block iv,
                                               DecryptBlock&& decrypt_block) noexcept
{
    if ((bytes.size() % cipher::Block{}.size()) != 0) {
        return false;
    }

    for (std::size_t offset = 0; offset < bytes.size(); offset += 8U) {
        const auto ciphertext = load_block(bytes, offset);
        const auto plaintext = cipher::xor_block(decrypt_block(ciphertext), iv);
        store_block(bytes, offset, plaintext);
        iv = ciphertext;
    }
    return true;
}

} // namespace detail

// Small, allocation-free CBC helpers used by the host-side MagicGate format
// code. They deliberately require complete 8-byte blocks. KELF headers, BIT
// tables and signatures are block-aligned; accepting implicit padding here
// would silently create a different wire format.
[[nodiscard]] constexpr bool des_encrypt_in_place(
    std::span<std::byte> bytes,
    const cipher::Block& key,
    cipher::Block iv = {}) noexcept
{
    return detail::encrypt_in_place(bytes, iv, [&](const cipher::Block& block) {
        return cipher::des_encrypt(block, key);
    });
}

[[nodiscard]] constexpr bool des_decrypt_in_place(
    std::span<std::byte> bytes,
    const cipher::Block& key,
    cipher::Block iv = {}) noexcept
{
    return detail::decrypt_in_place(bytes, iv, [&](const cipher::Block& block) {
        return cipher::des_decrypt(block, key);
    });
}

[[nodiscard]] constexpr bool tdes2_encrypt_in_place(
    std::span<std::byte> bytes,
    const cipher::DoubleKey& key,
    cipher::Block iv = {}) noexcept
{
    return detail::encrypt_in_place(bytes, iv, [&](const cipher::Block& block) {
        return cipher::tdes2_encrypt(block, key);
    });
}

[[nodiscard]] constexpr bool tdes2_decrypt_in_place(
    std::span<std::byte> bytes,
    const cipher::DoubleKey& key,
    cipher::Block iv = {}) noexcept
{
    return detail::decrypt_in_place(bytes, iv, [&](const cipher::Block& block) {
        return cipher::tdes2_decrypt(block, key);
    });
}

} // namespace ps2hdd::magicgate::cbc
