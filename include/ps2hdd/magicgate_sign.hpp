#pragma once

#include "ps2hdd/magicgate_payload.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::magicgate {

struct KelfSignBlock {
    std::uint32_t size{};
    std::uint32_t flags{};
};

struct DiskKelfSignPlan {
    KelfHeader header;
    std::vector<KelfSignBlock> blocks;
    DiskContentKeys content_keys;
    std::uint32_t signed_flag{};
    std::uint32_t encrypted_flag{};
};

struct DiskKelfSignResult {
    bool ok{};
    std::string error;
    std::vector<std::byte> file;
    KelfHeaderVerifyResult envelope;
};

namespace sign_detail {

constexpr void store_u32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

constexpr void store_block(std::span<std::byte> bytes,
                           std::size_t offset,
                           const cipher::Block& block) noexcept
{
    for (std::size_t i = 0; i < block.size(); ++i) {
        bytes[offset + i] = block[i];
    }
}

[[nodiscard]] inline bool encrypt_content_block(
    std::span<std::byte> bytes,
    const std::array<std::byte, 16>& kc,
    unsigned key_count,
    const cipher::Block& iv) noexcept
{
    if (key_count == 1U) {
        return cbc::des_encrypt_in_place(bytes, payload_detail::first_des_key(kc), iv);
    }
    if (key_count == 2U) {
        cipher::DoubleKey key{};
        std::copy(kc.begin(), kc.end(), key.begin());
        return cbc::tdes2_encrypt_in_place(bytes, key, iv);
    }
    return false;
}

[[nodiscard]] inline SignedFlagMapping mapping_for(std::uint32_t flag) noexcept
{
    if (flag == 0x01U) {
        return SignedFlagMapping::bit_0x01;
    }
    if (flag == 0x02U) {
        return SignedFlagMapping::bit_0x02;
    }
    return SignedFlagMapping::unresolved;
}

} // namespace sign_detail

// Build the common low-layout disk KELF entirely on the host. This milestone is
// deliberately narrower than every KELF Sony ever shipped: variable pre-key
// fields and ICVPS2 remain unsupported until we have equally strong behavioral
// evidence for them. A small honest signer is more useful than a universal one
// that occasionally invents eight bytes and hopes the MechaCon is sentimental.
//
// The caller supplies Kbit/Kc explicitly. Random generation belongs to a higher
// service layer so deterministic tests, imported historical keys and future
// provider policies can all use the same byte-level signer.
[[nodiscard]] inline DiskKelfSignResult sign_low_layout_disk_kelf(
    const DiskKelfSignPlan& plan,
    std::span<const std::byte> plaintext,
    const MagicGateKeyset& keyset)
{
    DiskKelfSignResult result;
    if ((plan.signed_flag != 0x01U && plan.signed_flag != 0x02U) ||
        (plan.encrypted_flag != 0x01U && plan.encrypted_flag != 0x02U) ||
        plan.signed_flag == plan.encrypted_flag) {
        result.error = "MagicGate sign plan must assign 0x01 and 0x02 to distinct signed/encrypted roles";
        return result;
    }
    if (plan.blocks.empty() || plan.blocks.size() > kKelfMaxBitBlocks) {
        result.error = "MagicGate sign plan must contain between 1 and 63 BIT blocks";
        return result;
    }
    if ((plan.header.flags & 0x0001U) != 0U) {
        result.error = "MagicGate low-layout signer does not support the variable pre-key field";
        return result;
    }
    if ((plan.header.flags & 0x0002U) != 0U) {
        result.error = "MagicGate low-layout signer refuses ICVPS2 until its software algorithm is verified";
        return result;
    }
    if ((plan.header.flags & 0xF000U) != 0U) {
        result.error = "MagicGate low-layout signer requires the standard 8-byte pre-key header-signature slot";
        return result;
    }
    if (plaintext.empty() || plaintext.size() > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "MagicGate plaintext size is outside the 32-bit KELF content range";
        return result;
    }

    const auto key_count = static_cast<unsigned>((plan.header.flags >> 4U) & 0x03U);
    std::uint64_t planned_bytes = 0;
    bool has_encrypted = false;
    bool has_signed = false;
    bool signed_set_differs_from_encrypted_set = false;
    for (const auto& block : plan.blocks) {
        if (block.size == 0U || (block.flags & ~0x03U) != 0U) {
            result.error = "MagicGate BIT block has zero size or unsupported flag bits";
            return result;
        }
        const bool signed_block = (block.flags & plan.signed_flag) != 0U;
        const bool encrypted = (block.flags & plan.encrypted_flag) != 0U;
        has_signed = has_signed || signed_block;
        has_encrypted = has_encrypted || encrypted;
        signed_set_differs_from_encrypted_set =
            signed_set_differs_from_encrypted_set || (signed_block != encrypted);
        if ((signed_block || encrypted) && (block.size % 8U) != 0U) {
            result.error = "MagicGate signed/encrypted BIT block is not 8-byte aligned";
            return result;
        }
        planned_bytes += block.size;
        if (planned_bytes > plaintext.size()) {
            result.error = "MagicGate BIT block sizes exceed the supplied plaintext";
            return result;
        }
    }
    if (planned_bytes != plaintext.size()) {
        result.error = "MagicGate BIT block sizes do not consume the supplied plaintext";
        return result;
    }
    if (!has_signed) {
        result.error = "MagicGate signer requires at least one signed block so root semantics are verifiable";
        return result;
    }
    if (!signed_set_differs_from_encrypted_set) {
        result.error = "MagicGate sign plan cannot prove which BIT flag means signed; add a differentiating block";
        return result;
    }
    if (has_encrypted && key_count != 1U && key_count != 2U) {
        result.error = "MagicGate encrypted content requires a supported DES key count of 1 or 2";
        return result;
    }

    const auto bit_table_bytes = 8U + plan.blocks.size() * 16U;
    const auto header_bytes = 72U + bit_table_bytes + 16U;
    if (header_bytes > std::numeric_limits<std::uint16_t>::max()) {
        result.error = "MagicGate generated KELF header exceeds its 16-bit size field";
        return result;
    }

    auto header = plan.header;
    header.elf_size = static_cast<std::uint32_t>(plaintext.size());
    header.header_size = static_cast<std::uint16_t>(header_bytes);
    // KELF BIT_count belongs to the optional pre-key descriptor area. The
    // encrypted runtime BIT table we build below has its own block_count.
    header.bit_count = 0;

    result.file.assign(header_bytes + plaintext.size(), std::byte{0});
    const auto fixed_header = serialize_fixed_header(header);
    std::copy(fixed_header.begin(), fixed_header.end(), result.file.begin());

    const auto layout = inspect_kelf(result.file);
    if (!layout.ok || layout.content_key_offset != 40U || layout.bit_table_offset != 72U ||
        layout.payload_offset != header_bytes) {
        result.error = "MagicGate generated header does not reproduce the expected low-layout geometry";
        return result;
    }

    const auto header_sig = header_signature(header, keyset.signing);
    if (!header_sig.ok) {
        result.error = "MagicGate header signature generation failed: " + header_sig.error;
        return result;
    }
    sign_detail::store_block(result.file, layout.content_key_offset - 8U, header_sig.value);
    if (!write_wrapped_disk_content_keys(result.file, layout, plan.content_keys, keyset.disk)) {
        result.error = "MagicGate disk Kbit/Kc wrapping did not fit the generated KELF";
        return result;
    }

    std::vector<std::byte> plain_bit_table(bit_table_bytes, std::byte{0});
    sign_detail::store_u32(plain_bit_table.data(), static_cast<std::uint32_t>(header_bytes));
    plain_bit_table[4] = static_cast<std::byte>(plan.blocks.size());

    std::vector<cipher::Block> signed_signatures;
    signed_signatures.reserve(plan.blocks.size());
    std::size_t payload_cursor = 0;
    for (std::size_t i = 0; i < plan.blocks.size(); ++i) {
        const auto& block = plan.blocks[i];
        const auto descriptor_offset = 8U + i * 16U;
        sign_detail::store_u32(plain_bit_table.data() + descriptor_offset, block.size);
        sign_detail::store_u32(plain_bit_table.data() + descriptor_offset + 4U, block.flags);

        const bool signed_block = (block.flags & plan.signed_flag) != 0U;
        const bool encrypted = (block.flags & plan.encrypted_flag) != 0U;
        if (signed_block) {
            const auto mode = encrypted ? ContentSignatureMode::encrypted_signed
                                        : ContentSignatureMode::plain_signed;
            const auto signature = content_block_signature(
                plaintext.subspan(payload_cursor, block.size), mode, keyset.signing);
            if (!signature.ok) {
                result.error = "MagicGate content signature generation failed: " + signature.error;
                return result;
            }
            sign_detail::store_block(plain_bit_table, descriptor_offset + 8U, signature.value);
            signed_signatures.push_back(signature.value);
        }
        payload_cursor += block.size;
    }

    const auto bit_sig = bit_table_signature(
        plain_bit_table, plan.content_keys.kbit, plan.content_keys.kc, keyset.signing);
    if (!bit_sig.ok) {
        result.error = "MagicGate BIT signature generation failed: " + bit_sig.error;
        return result;
    }
    const auto root_sig = root_signature(header_sig.value, bit_sig.value,
                                         signed_signatures, keyset.signing);
    if (!root_sig.ok) {
        result.error = "MagicGate root signature generation failed: " + root_sig.error;
        return result;
    }

    auto encrypted_bit_table = plain_bit_table;
    if (!encrypt_plaintext_bit_table_in_place(
            encrypted_bit_table, plan.content_keys.kbit, keyset.signing)) {
        result.error = "MagicGate BIT table encryption failed";
        return result;
    }
    std::copy(encrypted_bit_table.begin(), encrypted_bit_table.end(),
              result.file.begin() + static_cast<std::ptrdiff_t>(layout.bit_table_offset));
    const auto bit_sig_offset = layout.bit_table_offset + bit_table_bytes;
    sign_detail::store_block(result.file, bit_sig_offset, bit_sig.value);
    sign_detail::store_block(result.file, bit_sig_offset + 8U, root_sig.value);

    std::copy(plaintext.begin(), plaintext.end(),
              result.file.begin() + static_cast<std::ptrdiff_t>(header_bytes));
    payload_cursor = 0;
    for (const auto& block : plan.blocks) {
        if ((block.flags & plan.encrypted_flag) != 0U) {
            auto bytes = std::span<std::byte>(result.file).subspan(
                header_bytes + payload_cursor, block.size);
            if (!sign_detail::encrypt_content_block(
                    bytes, plan.content_keys.kc, key_count, keyset.signing.content_iv)) {
                result.error = "MagicGate content encryption failed";
                return result;
            }
        }
        payload_cursor += block.size;
    }

    // A signer that does not consume its own output is how format folklore is
    // born. Verify the exact bytes we are about to return through the separate
    // parser/verifier path and reject any disagreement immediately.
    result.envelope = verify_disk_kelf_header(result.file, keyset);
    if (!result.envelope.ok) {
        result.error = "MagicGate self-verification rejected the signed header: " +
                       result.envelope.error;
        return result;
    }
    if (result.envelope.signed_flag_mapping != sign_detail::mapping_for(plan.signed_flag)) {
        result.error = "MagicGate root signature resolved a different signed BIT flag than requested";
        return result;
    }
    const auto payload_check = verify_and_decrypt_disk_kelf_payload(
        result.file, result.envelope, keyset);
    if (!payload_check.ok || payload_check.plaintext.size() != plaintext.size() ||
        !std::equal(payload_check.plaintext.begin(), payload_check.plaintext.end(), plaintext.begin())) {
        result.error = payload_check.ok
                           ? "MagicGate self-verification produced different plaintext bytes"
                           : "MagicGate self-verification rejected the payload: " + payload_check.error;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::magicgate
