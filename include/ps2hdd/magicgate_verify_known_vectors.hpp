#pragma once

#include "ps2hdd/magicgate_known_vectors.hpp"
#include "ps2hdd/magicgate_verify.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace ps2hdd::magicgate::verify_known_vectors {

inline constexpr std::size_t kHeaderBytes = 128;
inline constexpr std::size_t kPayloadBytes = 96;
inline constexpr std::size_t kFileBytes = kHeaderBytes + kPayloadBytes;
inline constexpr std::size_t kHeaderSignatureOffset = 32;
inline constexpr std::size_t kContentKeyOffset = 40;
inline constexpr std::size_t kBitTableOffset = 72;
inline constexpr std::size_t kBitTableBytes = 40;
inline constexpr std::size_t kBitSignatureOffset = 112;
inline constexpr std::size_t kRootSignatureOffset = 120;

constexpr void store_u32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

template <std::size_t N, std::size_t M>
constexpr void copy_into(std::array<std::byte, N>& destination,
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
                          const cipher::Block& source) noexcept
{
    for (std::size_t i = 0; i < source.size(); ++i) {
        destination[offset + i] = source[i];
    }
}

[[nodiscard]] constexpr std::array<std::byte, kBitTableBytes>
make_plaintext_bit_table() noexcept
{
    std::array<std::byte, kBitTableBytes> table{};
    store_u32(table.data(), static_cast<std::uint32_t>(kHeaderBytes));
    table[4] = std::byte{2};

    // Descriptor 0 is deliberately flag 0x01, descriptor 1 flag 0x02. Their
    // signatures differ, so the root signature uniquely identifies which bit
    // this synthetic file treats as the signed-block selector.
    store_u32(table.data() + 8, 32);
    store_u32(table.data() + 12, 0x01);
    copy_block(table, 16, known_vectors::kSignedBlockSignatures[0]);

    store_u32(table.data() + 24, 64);
    store_u32(table.data() + 28, 0x02);
    copy_block(table, 32, known_vectors::kSignedBlockSignatures[1]);
    return table;
}

[[nodiscard]] constexpr KelfHeader make_header() noexcept
{
    KelfHeader header;
    for (std::size_t i = 0; i < header.user_header.size(); ++i) {
        header.user_header[i] = static_cast<std::byte>(0x51U + i * 3U);
    }
    header.elf_size = static_cast<std::uint32_t>(kPayloadBytes);
    header.header_size = static_cast<std::uint16_t>(kHeaderBytes);
    header.unknown5 = 0x0701;
    header.flags = 0; // low layout, no ICVPS2
    header.bit_count = 0;
    header.mg_zones = 1;
    return header;
}

[[nodiscard]] inline std::array<std::byte, kFileBytes> make_disk_kelf_envelope()
{
    std::array<std::byte, kFileBytes> file{};
    const auto header = make_header();
    copy_into(file, 0, serialize_fixed_header(header));

    const auto layout = inspect_kelf(file);
    if (!layout.ok || layout.content_key_offset != kContentKeyOffset ||
        layout.bit_table_offset != kBitTableOffset) {
        return {};
    }

    const auto& keyset = known_vectors::kSyntheticKeyset;
    DiskContentKeys keys;
    keys.kbit = known_vectors::kSyntheticKbit;
    keys.kc = known_vectors::kSyntheticKc;

    const auto header_sig = header_signature(header, keyset.signing);
    if (!header_sig.ok) {
        return {};
    }
    copy_block(file, kHeaderSignatureOffset, header_sig.value);

    if (!write_wrapped_disk_content_keys(file, layout, keys, keyset.disk)) {
        return {};
    }

    auto plaintext_bit_table = make_plaintext_bit_table();
    const auto bit_sig = bit_table_signature(
        plaintext_bit_table, keys.kbit, keys.kc, keyset.signing);
    if (!bit_sig.ok) {
        return {};
    }

    auto encrypted_bit_table = plaintext_bit_table;
    if (!encrypt_plaintext_bit_table_in_place(
            encrypted_bit_table, keys.kbit, keyset.signing)) {
        return {};
    }
    copy_into(file, kBitTableOffset, encrypted_bit_table);
    copy_block(file, kBitSignatureOffset, bit_sig.value);

    const std::array<cipher::Block, 1> signed_blocks{
        known_vectors::kSignedBlockSignatures[0]};
    const auto root_sig = root_signature(
        header_sig.value, bit_sig.value, signed_blocks, keyset.signing);
    if (!root_sig.ok) {
        return {};
    }
    copy_block(file, kRootSignatureOffset, root_sig.value);

    for (std::size_t i = 0; i < kPayloadBytes; ++i) {
        file[kHeaderBytes + i] = static_cast<std::byte>((i * 17U + 0x2dU) & 0xffU);
    }
    return file;
}

inline const auto kDiskKelfEnvelope = make_disk_kelf_envelope();

[[nodiscard]] inline bool rejects_damaged_header_signature()
{
    auto damaged = kDiskKelfEnvelope;
    damaged[kHeaderSignatureOffset] ^= std::byte{0x01};
    return !verify_disk_kelf_header(damaged, known_vectors::kSyntheticKeyset).ok;
}

[[nodiscard]] inline bool rejects_damaged_bit_ciphertext()
{
    auto damaged = kDiskKelfEnvelope;
    damaged[kBitTableOffset + 7U] ^= std::byte{0x80};
    return !verify_disk_kelf_header(damaged, known_vectors::kSyntheticKeyset).ok;
}

[[nodiscard]] inline bool rejects_damaged_root_signature()
{
    auto damaged = kDiskKelfEnvelope;
    damaged[kRootSignatureOffset + 3U] ^= std::byte{0x40};
    return !verify_disk_kelf_header(damaged, known_vectors::kSyntheticKeyset).ok;
}

} // namespace ps2hdd::magicgate::verify_known_vectors
