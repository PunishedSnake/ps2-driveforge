#pragma once

#include "ps2hdd/magicgate_disk_keys.hpp"
#include "ps2hdd/magicgate_signatures.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::magicgate {

enum class SignedFlagMapping {
    unresolved,
    bit_0x01,
    bit_0x02,
    ambiguous,
};

struct KelfHeaderVerifyResult {
    bool ok{};
    std::string error;
    KelfLayout layout;
    DiskContentKeys content_keys;
    BitTable bit_table;
    bool header_signature_valid{};
    bool bit_table_signature_valid{};
    bool root_signature_valid{};
    SignedFlagMapping signed_flag_mapping{SignedFlagMapping::unresolved};
    bool uses_icvps2{};
    cipher::Block icvps2{};
    std::size_t encrypted_bit_table_offset{};
    std::size_t encrypted_bit_table_bytes{};
};

namespace verify_detail {

[[nodiscard]] inline cipher::Block block_at(std::span<const std::byte> bytes,
                                            std::size_t offset) noexcept
{
    cipher::Block block{};
    if (offset > bytes.size() || block.size() > bytes.size() - offset) {
        return block;
    }
    for (std::size_t i = 0; i < block.size(); ++i) {
        block[i] = bytes[offset + i];
    }
    return block;
}

[[nodiscard]] inline std::size_t collect_block_signatures(
    const BitTable& table,
    std::uint32_t flag,
    std::array<cipher::Block, kKelfMaxBitBlocks>& out) noexcept
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < table.block_count; ++i) {
        if ((table.blocks[i].flags & flag) != 0) {
            out[count++] = table.blocks[i].signature;
        }
    }
    return count;
}

} // namespace verify_detail

// Verify the cryptographic header envelope of a disk KELF entirely on the host.
// This deliberately stops before decrypting payload blocks: the historical BIT
// flag naming disagreement must be resolved with real golden samples before a
// public API is allowed to decide which blocks receive content decryption.
//
// Current supported envelope is the common low-layout disk KELF where the
// 8-byte header signature immediately precedes Kbit/Kc. ICVPS2, when requested
// by KELF header flag bit 1, is the final 8 bytes of KELF_header_size exactly as
// SECRMAN's get_icvps2/store_icvps2 contract specifies.
[[nodiscard]] inline KelfHeaderVerifyResult verify_disk_kelf_header(
    std::span<const std::byte> file,
    const MagicGateKeyset& keyset)
{
    KelfHeaderVerifyResult result;
    result.layout = inspect_kelf(file);
    if (!result.layout.ok) {
        result.error = result.layout.error;
        return result;
    }

    const auto& header = result.layout.header;
    if ((header.flags & 0xF000U) != 0) {
        result.error = "MagicGate verifier currently requires the low-layout disk KELF header signature";
        return result;
    }
    if (result.layout.content_key_offset < 8U) {
        result.error = "MagicGate KELF has no room for the low-layout header signature";
        return result;
    }

    result.uses_icvps2 = ((header.flags >> 1U) & 1U) != 0;
    const std::size_t trailer_bytes = result.uses_icvps2 ? 24U : 16U;
    if (header.header_size < result.layout.bit_table_offset ||
        trailer_bytes > header.header_size - result.layout.bit_table_offset) {
        result.error = "MagicGate KELF header is too small for BIT/root signature trailer";
        return result;
    }

    result.encrypted_bit_table_offset = result.layout.bit_table_offset;
    result.encrypted_bit_table_bytes =
        static_cast<std::size_t>(header.header_size) - result.layout.bit_table_offset - trailer_bytes;
    if (result.encrypted_bit_table_bytes < kKelfBitTableHeaderBytes ||
        (result.encrypted_bit_table_bytes % 8U) != 0) {
        result.error = "MagicGate encrypted BIT table has an invalid byte length";
        return result;
    }

    const auto header_signature_offset = result.layout.content_key_offset - 8U;
    const auto stored_header_signature = verify_detail::block_at(file, header_signature_offset);
    const auto calculated_header_signature = header_signature(header, keyset.signing);
    result.header_signature_valid = calculated_header_signature.ok &&
                                    calculated_header_signature.value == stored_header_signature;
    if (!result.header_signature_valid) {
        result.error = "MagicGate KELF header signature does not verify";
        return result;
    }

    const auto unwrapped = unwrap_disk_content_keys(file, keyset.disk);
    if (!unwrapped.ok) {
        result.error = "MagicGate disk content keys could not be unwrapped: " + unwrapped.error;
        return result;
    }
    result.content_keys = unwrapped.keys;

    std::vector<std::byte> plaintext_bit_table(result.encrypted_bit_table_bytes);
    for (std::size_t i = 0; i < plaintext_bit_table.size(); ++i) {
        plaintext_bit_table[i] = file[result.encrypted_bit_table_offset + i];
    }
    if (!decrypt_bit_table_in_place(plaintext_bit_table,
                                    result.content_keys.kbit,
                                    keyset.signing)) {
        result.error = "MagicGate encrypted BIT table could not be decrypted";
        return result;
    }

    result.bit_table = parse_plaintext_bit_table(plaintext_bit_table);
    if (!result.bit_table.ok) {
        result.error = "MagicGate plaintext BIT table is invalid: " + result.bit_table.error;
        return result;
    }
    if (result.bit_table.header_size != header.header_size) {
        result.error = "MagicGate BIT header_size does not match the KELF header";
        return result;
    }

    std::uint64_t content_bytes = 0;
    for (std::size_t i = 0; i < result.bit_table.block_count; ++i) {
        const auto size = static_cast<std::uint64_t>(result.bit_table.blocks[i].size);
        if (size > std::numeric_limits<std::uint64_t>::max() - content_bytes) {
            result.error = "MagicGate BIT content size overflow";
            return result;
        }
        content_bytes += size;
    }
    if (content_bytes != header.elf_size) {
        result.error = "MagicGate BIT block sizes do not sum to KELF ELF_size";
        return result;
    }

    const auto stored_bit_signature_offset =
        result.encrypted_bit_table_offset + result.encrypted_bit_table_bytes;
    const auto stored_bit_signature = verify_detail::block_at(file, stored_bit_signature_offset);
    const auto calculated_bit_signature = bit_table_signature(
        plaintext_bit_table,
        result.content_keys.kbit,
        result.content_keys.kc,
        keyset.signing);
    result.bit_table_signature_valid = calculated_bit_signature.ok &&
                                       calculated_bit_signature.value == stored_bit_signature;
    if (!result.bit_table_signature_valid) {
        result.error = "MagicGate BIT table signature does not verify";
        return result;
    }

    const auto stored_root_signature = verify_detail::block_at(file, stored_bit_signature_offset + 8U);
    std::array<cipher::Block, kKelfMaxBitBlocks> bit01_signatures{};
    std::array<cipher::Block, kKelfMaxBitBlocks> bit02_signatures{};
    const auto bit01_count = verify_detail::collect_block_signatures(
        result.bit_table, 0x01U, bit01_signatures);
    const auto bit02_count = verify_detail::collect_block_signatures(
        result.bit_table, 0x02U, bit02_signatures);

    const auto root01 = root_signature(
        calculated_header_signature.value,
        calculated_bit_signature.value,
        std::span<const cipher::Block>(bit01_signatures).first(bit01_count),
        keyset.signing);
    const auto root02 = root_signature(
        calculated_header_signature.value,
        calculated_bit_signature.value,
        std::span<const cipher::Block>(bit02_signatures).first(bit02_count),
        keyset.signing);
    const bool match01 = root01.ok && root01.value == stored_root_signature;
    const bool match02 = root02.ok && root02.value == stored_root_signature;

    result.root_signature_valid = match01 || match02;
    if (!result.root_signature_valid) {
        result.error = "MagicGate root signature does not verify under either public BIT signed-flag interpretation";
        return result;
    }
    if (match01 && match02) {
        result.signed_flag_mapping = SignedFlagMapping::ambiguous;
    } else {
        result.signed_flag_mapping = match01 ? SignedFlagMapping::bit_0x01
                                             : SignedFlagMapping::bit_0x02;
    }

    if (result.uses_icvps2) {
        result.icvps2 = verify_detail::block_at(file, header.header_size - 8U);
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::magicgate
