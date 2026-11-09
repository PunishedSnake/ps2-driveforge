#pragma once

#include "ps2hdd/magicgate_cbc.hpp"
#include "ps2hdd/magicgate_kelf.hpp"
#include "ps2hdd/magicgate_keyset.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ps2hdd::magicgate {

struct BitBlock {
    std::uint32_t size{};
    std::uint32_t flags{};
    cipher::Block signature{};
};

struct BitTable {
    bool ok{};
    std::string error;
    std::uint32_t header_size{};
    std::uint8_t block_count{};
    std::array<BitBlock, kKelfMaxBitBlocks> blocks{};
    std::size_t serialized_bytes{};
};

struct SignatureResult {
    bool ok{};
    std::string error;
    cipher::Block value{};
};

namespace signature_detail {

constexpr void store_le16(std::byte* p, std::uint16_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

constexpr void store_le32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] constexpr std::uint32_t load_le32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

[[nodiscard]] constexpr cipher::Block load_block(std::span<const std::byte> bytes,
                                                  std::size_t offset) noexcept
{
    cipher::Block block{};
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = bytes[offset + i];
    }
    return block;
}

[[nodiscard]] constexpr cipher::Block key_half(
    const std::array<std::byte, 16>& key,
    std::size_t offset) noexcept
{
    cipher::Block block{};
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = key[offset + i];
    }
    return block;
}

[[nodiscard]] constexpr cipher::DoubleKey join_keys(const cipher::Block& first,
                                                     const cipher::Block& second) noexcept
{
    cipher::DoubleKey key{};
    for (std::size_t i = 0; i < first.size(); ++i) {
        key[i] = first[i];
        key[i + first.size()] = second[i];
    }
    return key;
}

[[nodiscard]] constexpr bool equal_block(const cipher::Block& a,
                                         const cipher::Block& b) noexcept
{
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

} // namespace signature_detail

[[nodiscard]] constexpr std::array<std::byte, kKelfFixedHeaderBytes>
serialize_fixed_header(const KelfHeader& header) noexcept
{
    std::array<std::byte, kKelfFixedHeaderBytes> bytes{};
    std::copy(header.user_header.begin(), header.user_header.end(), bytes.begin());
    signature_detail::store_le32(bytes.data() + 0x10, header.elf_size);
    signature_detail::store_le16(bytes.data() + 0x14, header.header_size);
    signature_detail::store_le16(bytes.data() + 0x16, header.unknown5);
    signature_detail::store_le16(bytes.data() + 0x18, header.flags);
    signature_detail::store_le16(bytes.data() + 0x1a, header.bit_count);
    signature_detail::store_le32(bytes.data() + 0x1c, header.mg_zones);
    return bytes;
}

// Parse the decrypted BIT table returned by SECRMAN. The table is exactly an
// 8-byte header followed by block_count 16-byte descriptors. This parser does
// not assign meaning to individual flag bits yet: historical public sources
// disagree on the labels for bits 0 and 1, so interpretation remains a later,
// separately verified layer instead of becoming accidental ABI.
[[nodiscard]] constexpr BitTable parse_plaintext_bit_table(std::span<const std::byte> bytes)
{
    BitTable result;
    if (bytes.size() < kKelfBitTableHeaderBytes || (bytes.size() % 8U) != 0) {
        result.error = "MagicGate BIT table must contain complete 8-byte blocks";
        return result;
    }

    result.header_size = signature_detail::load_le32(bytes.data());
    result.block_count = std::to_integer<std::uint8_t>(bytes[4]);
    if (result.block_count > kKelfMaxBitBlocks) {
        result.error = "MagicGate BIT table exceeds SECRMAN's 63-block limit";
        return result;
    }

    const auto expected = kKelfBitTableHeaderBytes +
                          static_cast<std::size_t>(result.block_count) * kKelfBitBlockBytes;
    if (bytes.size() != expected) {
        result.error = "MagicGate BIT table byte length does not match block_count";
        return result;
    }

    for (std::size_t i = 0; i < result.block_count; ++i) {
        const auto offset = kKelfBitTableHeaderBytes + i * kKelfBitBlockBytes;
        result.blocks[i].size = signature_detail::load_le32(bytes.data() + offset);
        result.blocks[i].flags = signature_detail::load_le32(bytes.data() + offset + 4U);
        result.blocks[i].signature = signature_detail::load_block(bytes, offset + 8U);
    }

    result.serialized_bytes = expected;
    result.ok = true;
    return result;
}

[[nodiscard]] constexpr SignatureResult header_signature(
    const KelfHeader& header,
    const SigningKeyset& keyset) noexcept
{
    SignatureResult result;
    auto bytes = serialize_fixed_header(header);
    if (!cbc::des_encrypt_in_place(bytes, keyset.signature_master)) {
        result.error = "MagicGate fixed header is not DES block aligned";
        return result;
    }

    auto signature = signature_detail::load_block(bytes, bytes.size() - 8U);
    signature = cipher::des_decrypt(signature, keyset.signature_hash);
    signature = cipher::des_encrypt(signature, keyset.signature_master);
    result.value = signature;
    result.ok = true;
    return result;
}

[[nodiscard]] constexpr SignatureResult bit_table_signature(
    std::span<const std::byte> plaintext_table,
    const std::array<std::byte, 16>& kbit,
    const std::array<std::byte, 16>& kc,
    const SigningKeyset& keyset) noexcept
{
    SignatureResult result;
    if (plaintext_table.empty() || (plaintext_table.size() % 8U) != 0) {
        result.error = "MagicGate BIT signature requires complete non-empty 8-byte blocks";
        return result;
    }

    auto hash = signature_detail::key_half(kbit, 0);
    const auto kbit_second = signature_detail::key_half(kbit, 8);
    if (!signature_detail::equal_block(hash, kbit_second)) {
        hash = cipher::xor_block(hash, kbit_second);
    }

    const auto kc_first = signature_detail::key_half(kc, 0);
    hash = cipher::xor_block(hash, kc_first);
    const auto kc_second = signature_detail::key_half(kc, 8);
    if (!signature_detail::equal_block(kc_first, kc_second)) {
        hash = cipher::xor_block(hash, kc_second);
    }

    for (std::size_t offset = 0; offset < plaintext_table.size(); offset += 8U) {
        hash = cipher::xor_block(hash, signature_detail::load_block(plaintext_table, offset));
    }

    const auto signing_key = signature_detail::join_keys(
        keyset.signature_master, keyset.signature_hash);
    result.value = cipher::tdes2_encrypt(hash, signing_key);
    result.ok = true;
    return result;
}

[[nodiscard]] constexpr SignatureResult root_signature(
    const cipher::Block& header_sig,
    const cipher::Block& bit_table_sig,
    std::span<const cipher::Block> signed_block_signatures,
    const SigningKeyset& keyset) noexcept
{
    SignatureResult result;
    cipher::Block chain{};
    const auto absorb = [&](const cipher::Block& block, cipher::Block& state) constexpr {
        state = cipher::des_encrypt(cipher::xor_block(block, state),
                                    keyset.root_signature_master);
    };

    absorb(header_sig, chain);
    absorb(bit_table_sig, chain);
    for (const auto& block_signature : signed_block_signatures) {
        absorb(block_signature, chain);
    }

    result.value = cipher::tdes2_decrypt(chain, keyset.root_signature_hash);
    result.ok = true;
    return result;
}

[[nodiscard]] constexpr bool encrypt_plaintext_bit_table_in_place(
    std::span<std::byte> plaintext_table,
    const std::array<std::byte, 16>& kbit,
    const SigningKeyset& keyset) noexcept
{
    return cbc::tdes2_encrypt_in_place(plaintext_table, kbit, keyset.content_table_iv);
}

[[nodiscard]] constexpr bool decrypt_bit_table_in_place(
    std::span<std::byte> encrypted_table,
    const std::array<std::byte, 16>& kbit,
    const SigningKeyset& keyset) noexcept
{
    return cbc::tdes2_decrypt_in_place(encrypted_table, kbit, keyset.content_table_iv);
}

} // namespace ps2hdd::magicgate
