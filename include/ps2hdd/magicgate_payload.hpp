#pragma once

#include "ps2hdd/magicgate_content.hpp"
#include "ps2hdd/magicgate_verify.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::magicgate {

struct PayloadBlockStatus {
    std::uint32_t flags{};
    std::size_t offset{};
    std::size_t size{};
    bool encrypted{};
    bool signed_block{};
    bool signature_valid{};
};

struct KelfPayloadVerifyResult {
    bool ok{};
    std::string error;
    std::vector<std::byte> plaintext;
    std::vector<PayloadBlockStatus> blocks;
    std::uint32_t signed_flag{};
    std::uint32_t encrypted_flag{};
    unsigned content_key_count{};
    bool icvps2_present{};
    bool icvps2_verified{};
};

namespace payload_detail {

[[nodiscard]] inline cipher::Block first_des_key(
    const std::array<std::byte, 16>& key) noexcept
{
    cipher::Block out{};
    std::copy_n(key.begin(), out.size(), out.begin());
    return out;
}

[[nodiscard]] inline bool decrypt_content_block(
    std::span<std::byte> bytes,
    const std::array<std::byte, 16>& kc,
    unsigned key_count,
    const cipher::Block& iv) noexcept
{
    if (key_count == 1U) {
        return cbc::des_decrypt_in_place(bytes, first_des_key(kc), iv);
    }
    if (key_count == 2U) {
        cipher::DoubleKey key{};
        std::copy(kc.begin(), kc.end(), key.begin());
        return cbc::tdes2_decrypt_in_place(bytes, key, iv);
    }
    return false;
}

} // namespace payload_detail

// Verify and decrypt the payload only after the header envelope has selected an
// unambiguous signed-bit interpretation. Public PS2 references disagree on the
// names of BIT flag 0x01 and 0x02, while the root signature tells us which one
// actually contributes block signatures for this file. Evidence beats comments.
//
// The opposite flag is then treated as the encrypted selector. Each encrypted
// BIT block starts with the configured content IV, matching the host KELF
// behavioral reference. CBC state does not leak from one descriptor into the
// next merely because doing so would look tidier in a generic crypto API.
[[nodiscard]] inline KelfPayloadVerifyResult verify_and_decrypt_disk_kelf_payload(
    std::span<const std::byte> file,
    const KelfHeaderVerifyResult& envelope,
    const MagicGateKeyset& keyset)
{
    KelfPayloadVerifyResult result;
    if (!envelope.ok) {
        result.error = "MagicGate payload verification requires a verified KELF header envelope";
        return result;
    }

    if (envelope.signed_flag_mapping == SignedFlagMapping::bit_0x01) {
        result.signed_flag = 0x01U;
        result.encrypted_flag = 0x02U;
    } else if (envelope.signed_flag_mapping == SignedFlagMapping::bit_0x02) {
        result.signed_flag = 0x02U;
        result.encrypted_flag = 0x01U;
    } else {
        result.error = "MagicGate BIT signed/encrypted mapping is ambiguous; payload crypto is refused";
        return result;
    }

    result.content_key_count = (envelope.layout.header.flags >> 4U) & 0x03U;
    result.icvps2_present = envelope.uses_icvps2;
    // ICVPS2 generation/verification is a separate MechaCon-derived invariant.
    // Do not turn presence into fake verification just because the bytes parse.
    result.icvps2_verified = !result.icvps2_present;

    const auto payload_offset = static_cast<std::size_t>(envelope.layout.header.header_size);
    const auto payload_bytes = static_cast<std::size_t>(envelope.layout.header.elf_size);
    if (payload_offset > file.size() || payload_bytes > file.size() - payload_offset) {
        result.error = "MagicGate payload extends beyond the supplied KELF";
        return result;
    }

    result.plaintext.assign(file.begin() + static_cast<std::ptrdiff_t>(payload_offset),
                            file.begin() + static_cast<std::ptrdiff_t>(payload_offset + payload_bytes));
    result.blocks.reserve(envelope.bit_table.block_count);

    std::size_t cursor = 0;
    for (std::size_t i = 0; i < envelope.bit_table.block_count; ++i) {
        const auto& descriptor = envelope.bit_table.blocks[i];
        const auto block_bytes = static_cast<std::size_t>(descriptor.size);
        if (block_bytes == 0U || cursor > result.plaintext.size() ||
            block_bytes > result.plaintext.size() - cursor) {
            result.error = "MagicGate BIT block geometry exceeds the KELF payload";
            return result;
        }

        PayloadBlockStatus status;
        status.flags = descriptor.flags;
        status.offset = cursor;
        status.size = block_bytes;
        status.encrypted = (descriptor.flags & result.encrypted_flag) != 0;
        status.signed_block = (descriptor.flags & result.signed_flag) != 0;

        auto block = std::span<std::byte>(result.plaintext).subspan(cursor, block_bytes);
        if (status.encrypted) {
            if ((block_bytes % 8U) != 0U) {
                result.error = "MagicGate encrypted BIT block is not 8-byte aligned";
                return result;
            }
            if (result.content_key_count != 1U && result.content_key_count != 2U) {
                result.error = "MagicGate encrypted payload requests an unsupported DES key count";
                return result;
            }
            if (!payload_detail::decrypt_content_block(
                    block,
                    envelope.content_keys.kc,
                    result.content_key_count,
                    keyset.signing.content_iv)) {
                result.error = "MagicGate content block decryption failed";
                return result;
            }
        }

        if (status.signed_block) {
            const auto mode = status.encrypted
                                  ? ContentSignatureMode::encrypted_signed
                                  : ContentSignatureMode::plain_signed;
            const auto signature = content_block_signature(
                std::span<const std::byte>(block.data(), block.size()), mode, keyset.signing);
            if (!signature.ok) {
                result.error = "MagicGate content block signature could not be calculated: " +
                               signature.error;
                return result;
            }
            status.signature_valid = signature.value == descriptor.signature;
            if (!status.signature_valid) {
                result.error = "MagicGate content block signature does not verify";
                return result;
            }
        } else {
            status.signature_valid = true;
        }

        result.blocks.push_back(status);
        cursor += block_bytes;
    }

    if (cursor != result.plaintext.size()) {
        result.error = "MagicGate BIT block sizes do not consume the complete payload";
        return result;
    }

    if (result.icvps2_present) {
        result.error = "MagicGate payload signatures verify, but ICVPS2 verification is not implemented yet";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::magicgate
