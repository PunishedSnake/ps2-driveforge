#pragma once

#include "ps2hdd/magicgate_signatures.hpp"

#include <cstddef>
#include <span>

namespace ps2hdd::magicgate {

enum class ContentSignatureMode {
    plain_signed,
    encrypted_signed,
};

// Compute the signature over plaintext block bytes. MagicGate uses two
// different constructions depending on how the BIT descriptor classifies the
// block. The caller supplies that classification explicitly; this layer does
// not guess historical BIT flag semantics.
[[nodiscard]] constexpr SignatureResult content_block_signature(
    std::span<const std::byte> plaintext,
    ContentSignatureMode mode,
    const SigningKeyset& keyset) noexcept
{
    SignatureResult result;
    if (plaintext.empty() || (plaintext.size() % 8U) != 0) {
        result.error = "MagicGate content signature requires complete non-empty 8-byte blocks";
        return result;
    }

    if (mode == ContentSignatureMode::encrypted_signed) {
        cipher::Block hash{};
        for (std::size_t offset = 0; offset < plaintext.size(); offset += 8U) {
            hash = cipher::xor_block(hash,
                                     signature_detail::load_block(plaintext, offset));
        }
        const auto signing_key = signature_detail::join_keys(
            keyset.signature_master, keyset.signature_hash);
        result.value = cipher::tdes2_encrypt(hash, signing_key);
        result.ok = true;
        return result;
    }

    cipher::Block chain{};
    for (std::size_t offset = 0; offset < plaintext.size(); offset += 8U) {
        chain = cipher::des_encrypt(
            cipher::xor_block(signature_detail::load_block(plaintext, offset), chain),
            keyset.signature_master);
    }
    chain = cipher::des_decrypt(chain, keyset.signature_hash);
    result.value = cipher::des_encrypt(chain, keyset.signature_master);
    result.ok = true;
    return result;
}

[[nodiscard]] constexpr bool verify_content_block_signature(
    std::span<const std::byte> plaintext,
    ContentSignatureMode mode,
    const cipher::Block& expected,
    const SigningKeyset& keyset) noexcept
{
    const auto calculated = content_block_signature(plaintext, mode, keyset);
    return calculated.ok && calculated.value == expected;
}

} // namespace ps2hdd::magicgate
