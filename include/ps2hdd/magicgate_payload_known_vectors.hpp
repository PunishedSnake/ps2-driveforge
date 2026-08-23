#pragma once

#include "ps2hdd/magicgate_known_vectors.hpp"
#include "ps2hdd/magicgate_payload.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace ps2hdd::magicgate::payload_known_vectors {

inline constexpr std::size_t kHeaderBytes = 144U;
inline constexpr std::size_t kPayloadBytes = 96U;
inline constexpr std::size_t kFileBytes = kHeaderBytes + kPayloadBytes;
inline constexpr std::size_t kHeaderSignatureOffset = 32U;
inline constexpr std::size_t kContentKeyOffset = 40U;
inline constexpr std::size_t kBitTableOffset = 72U;
inline constexpr std::size_t kBitTableBytes = 56U;
inline constexpr std::size_t kBitSignatureOffset = 128U;
inline constexpr std::size_t kRootSignatureOffset = 136U;

constexpr void store_u32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

template <std::size_t N, std::size_t M>
constexpr void copy_array(std::array<std::byte, N>& destination,
                          std::size_t offset,
                          const std::array<std::byte, M>& source) noexcept
{
    for (std::size_t i = 0; i < source.size(); ++i) {
        destination[offset + i] = source[i];
    }
}

template <std::size_t N>
constexpr void copy_block(std::array<std::byte, N>& destination,
                          std::size_t offset,
                          const cipher::Block& block) noexcept
{
    for (std::size_t i = 0; i < block.size(); ++i) {
        destination[offset + i] = block[i];
    }
}

[[nodiscard]] constexpr KelfHeader make_header() noexcept
{
    KelfHeader header;
    for (std::size_t i = 0; i < header.user_header.size(); ++i) {
        header.user_header[i] = static_cast<std::byte>(0x71U + i * 5U);
    }
    header.elf_size = static_cast<std::uint32_t>(kPayloadBytes);
    header.header_size = static_cast<std::uint16_t>(kHeaderBytes);
    header.unknown5 = 0x0701U;
    // Bits 4-5 select two DES keys for encrypted content in the public host
    // KELF behavioral reference. Header bit 1 remains clear, so this vector
    // deliberately does not pretend to test ICVPS2 yet.
    header.flags = 0x0020U;
    header.bit_count = 0;
    header.mg_zones = 1U;
    return header;
}

[[nodiscard]] constexpr std::array<std::byte, kPayloadBytes>
make_plaintext_payload() noexcept
{
    std::array<std::byte, kPayloadBytes> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((0x39U + i * 29U) & 0xffU);
    }
    return bytes;
}

[[nodiscard]] constexpr std::array<std::byte, kBitTableBytes>
make_plain_bit_table(const std::array<std::byte, kPayloadBytes>& plaintext,
                     const SigningKeyset& keyset) noexcept
{
    std::array<std::byte, kBitTableBytes> table{};
    store_u32(table.data(), static_cast<std::uint32_t>(kHeaderBytes));
    table[4] = std::byte{3};

    // 0x02 is intentionally the signed selector in this synthetic file. The
    // root signature, not a hard-coded public comment, is what lets the verifier
    // prove that fact later.
    constexpr std::uint32_t both = 0x03U;
    constexpr std::uint32_t signed_only = 0x02U;
    constexpr std::uint32_t encrypted_only = 0x01U;

    const auto first = content_block_signature(
        std::span<const std::byte>(plaintext).subspan(0, 32),
        ContentSignatureMode::encrypted_signed,
        keyset);
    const auto second = content_block_signature(
        std::span<const std::byte>(plaintext).subspan(32, 32),
        ContentSignatureMode::plain_signed,
        keyset);
    if (!first.ok || !second.ok) {
        return {};
    }

    store_u32(table.data() + 8, 32U);
    store_u32(table.data() + 12, both);
    copy_block(table, 16, first.value);

    store_u32(table.data() + 24, 32U);
    store_u32(table.data() + 28, signed_only);
    copy_block(table, 32, second.value);

    store_u32(table.data() + 40, 32U);
    store_u32(table.data() + 44, encrypted_only);
    // The third block is not signed. Its signature field stays zero on purpose.
    return table;
}

[[nodiscard]] constexpr std::array<std::byte, kFileBytes>
make_complete_disk_kelf() noexcept
{
    std::array<std::byte, kFileBytes> file{};
    const auto& keyset = known_vectors::kParsedSyntheticKeyset.keyset;
    const auto header = make_header();
    const auto plaintext = make_plaintext_payload();
    copy_array(file, 0, serialize_fixed_header(header));

    const auto layout = inspect_kelf(file);
    if (!layout.ok || layout.content_key_offset != kContentKeyOffset ||
        layout.bit_table_offset != kBitTableOffset ||
        layout.payload_offset != kHeaderBytes) {
        return {};
    }

    const auto header_sig = header_signature(header, keyset.signing);
    if (!header_sig.ok) {
        return {};
    }
    copy_block(file, kHeaderSignatureOffset, header_sig.value);

    DiskContentKeys keys;
    keys.kbit = known_vectors::kSyntheticKbit;
    keys.kc = known_vectors::kSyntheticKc;
    if (!write_wrapped_disk_content_keys(file, layout, keys, keyset.disk)) {
        return {};
    }

    const auto plain_bit_table = make_plain_bit_table(plaintext, keyset.signing);
    const auto bit_sig = bit_table_signature(
        plain_bit_table, keys.kbit, keys.kc, keyset.signing);
    if (!bit_sig.ok) {
        return {};
    }

    auto encrypted_bit_table = plain_bit_table;
    if (!encrypt_plaintext_bit_table_in_place(
            encrypted_bit_table, keys.kbit, keyset.signing)) {
        return {};
    }
    copy_array(file, kBitTableOffset, encrypted_bit_table);
    copy_block(file, kBitSignatureOffset, bit_sig.value);

    const auto parsed_table = parse_plaintext_bit_table(plain_bit_table);
    if (!parsed_table.ok || parsed_table.block_count != 3U) {
        return {};
    }
    const std::array<cipher::Block, 2> signed_signatures{
        parsed_table.blocks[0].signature,
        parsed_table.blocks[1].signature};
    const auto root_sig = root_signature(
        header_sig.value, bit_sig.value, signed_signatures, keyset.signing);
    if (!root_sig.ok) {
        return {};
    }
    copy_block(file, kRootSignatureOffset, root_sig.value);

    copy_array(file, kHeaderBytes, plaintext);
    cipher::DoubleKey kc{};
    for (std::size_t i = 0; i < kc.size(); ++i) {
        kc[i] = keys.kc[i];
    }

    auto first = std::span<std::byte>(file).subspan(kHeaderBytes, 32);
    auto third = std::span<std::byte>(file).subspan(kHeaderBytes + 64U, 32);
    if (!cbc::tdes2_encrypt_in_place(first, kc, keyset.signing.content_iv) ||
        !cbc::tdes2_encrypt_in_place(third, kc, keyset.signing.content_iv)) {
        return {};
    }
    return file;
}

inline constexpr auto kPlaintextPayload = make_plaintext_payload();
inline constexpr auto kCompleteDiskKelf = make_complete_disk_kelf();
inline constexpr auto kEnvelope = verify_disk_kelf_header(
    kCompleteDiskKelf, known_vectors::kParsedSyntheticKeyset.keyset);
static_assert(kEnvelope.ok);
static_assert(kEnvelope.signed_flag_mapping == SignedFlagMapping::bit_0x02);

// Payload verification returns a vector, so it is exercised at runtime rather
// than forced through constexpr allocation. The envelope above still gives CI
// compile-time coverage for every cryptographic layer preceding the payload.

} // namespace ps2hdd::magicgate::payload_known_vectors
