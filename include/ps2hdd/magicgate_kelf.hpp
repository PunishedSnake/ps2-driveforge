#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace ps2hdd::magicgate {

inline constexpr std::size_t kKelfFixedHeaderBytes = 32;
inline constexpr std::size_t kKelfContentKeyBytes = 32;
inline constexpr std::size_t kKelfBitBlockBytes = 16;
inline constexpr std::size_t kKelfMaxBitBlocks = 63;
inline constexpr std::size_t kKelfBitTableHeaderBytes = 8;

struct KelfHeader {
    std::array<std::byte, 16> user_header{};
    std::uint32_t elf_size{};
    std::uint16_t header_size{};
    std::uint16_t unknown5{};
    std::uint16_t flags{};
    std::uint16_t bit_count{};
    std::uint32_t mg_zones{};
};

struct KelfLayout {
    bool ok{};
    std::string error;
    KelfHeader header;
    std::size_t content_key_offset{};
    std::size_t bit_table_offset{};
    std::size_t payload_offset{};
    std::size_t file_bytes{};
};

namespace detail {

[[nodiscard]] inline std::uint16_t load_le16(const std::byte* p) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[0])) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[1])) << 8U);
}

[[nodiscard]] inline std::uint32_t load_le32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

[[nodiscard]] inline bool checked_add(std::size_t a, std::size_t b, std::size_t& out) noexcept
{
    if (b > std::numeric_limits<std::size_t>::max() - a) {
        return false;
    }
    out = a + b;
    return true;
}

} // namespace detail

// Parse only the byte layout that SECRMAN itself consumes. This function does
// not decide whether a KELF is cryptographically valid; it proves that all
// offsets needed by a future MagicGate provider are inside one bounded file.
//
// The layout mirrors ps2sdk's SecrKELFHeader_t/get_BitTableOffset contract:
//  - 32-byte fixed header;
//  - BIT_count 16-byte descriptors immediately after it;
//  - optional variable field when flags bit 0 is set;
//  - an extra 8-byte field when the high flag nibble is zero;
//  - 32 bytes of Kbit/Kc;
//  - BIT table returned by SECRMAN immediately after those content keys.
//
// Keeping layout parsing key-free is deliberate. Inspection must remain useful
// on a PC that has no MagicGate keyset, and malformed input must be rejected
// before any cryptographic code is asked to touch it.
[[nodiscard]] inline KelfLayout inspect_kelf(std::span<const std::byte> file)
{
    KelfLayout result;
    if (file.size() < kKelfFixedHeaderBytes) {
        result.error = "KELF is smaller than the 32-byte fixed header";
        return result;
    }

    std::copy_n(file.begin(), result.header.user_header.size(),
                result.header.user_header.begin());
    result.header.elf_size = detail::load_le32(file.data() + 0x10);
    result.header.header_size = detail::load_le16(file.data() + 0x14);
    result.header.unknown5 = detail::load_le16(file.data() + 0x16);
    result.header.flags = detail::load_le16(file.data() + 0x18);
    result.header.bit_count = detail::load_le16(file.data() + 0x1a);
    result.header.mg_zones = detail::load_le32(file.data() + 0x1c);

    if (result.header.bit_count > kKelfMaxBitBlocks) {
        result.error = "KELF BIT_count exceeds SECRMAN's 63-block table limit";
        return result;
    }
    if (result.header.header_size < kKelfFixedHeaderBytes ||
        result.header.header_size > file.size()) {
        result.error = "KELF header_size is outside the supplied file";
        return result;
    }

    std::size_t cursor = kKelfFixedHeaderBytes;
    const auto bit_descriptor_bytes =
        static_cast<std::size_t>(result.header.bit_count) * kKelfBitBlockBytes;
    if (!detail::checked_add(cursor, bit_descriptor_bytes, cursor) ||
        cursor > result.header.header_size) {
        result.error = "KELF BIT descriptors extend beyond header_size";
        return result;
    }

    if ((result.header.flags & 0x0001U) != 0) {
        if (cursor >= result.header.header_size) {
            result.error = "KELF variable header field has no length byte";
            return result;
        }
        const auto variable_bytes =
            static_cast<std::size_t>(std::to_integer<unsigned char>(file[cursor])) + 1U;
        if (!detail::checked_add(cursor, variable_bytes, cursor) ||
            cursor > result.header.header_size) {
            result.error = "KELF variable header field exceeds header_size";
            return result;
        }
    }

    if ((result.header.flags & 0xF000U) == 0) {
        if (!detail::checked_add(cursor, 8U, cursor) || cursor > result.header.header_size) {
            result.error = "KELF low-layout 8-byte field exceeds header_size";
            return result;
        }
    }

    result.content_key_offset = cursor;
    if (!detail::checked_add(cursor, kKelfContentKeyBytes, cursor) ||
        cursor > result.header.header_size) {
        result.error = "KELF Kbit/Kc region extends beyond header_size";
        return result;
    }
    result.bit_table_offset = cursor;

    // The decrypted BIT table begins here after SECRMAN has processed the
    // header. For an on-disk KELF these bytes can still be encrypted, so parser
    // validation stops at geometry. Treating ciphertext as a plaintext BIT
    // table would make a very confident validator and a very bad one.
    if (result.bit_table_offset > result.header.header_size) {
        result.error = "KELF BIT table offset exceeds header_size";
        return result;
    }

    result.payload_offset = result.header.header_size;
    std::size_t complete = 0;
    if (!detail::checked_add(result.payload_offset,
                             static_cast<std::size_t>(result.header.elf_size), complete) ||
        complete > file.size()) {
        result.error = "KELF ELF payload extends beyond the supplied file";
        return result;
    }
    result.file_bytes = complete;
    result.ok = true;
    return result;
}

// Dynamic extent is intentional: malformed layout has a natural empty return
// value. Callers that need exactly 32 bytes should require layout.ok first.
[[nodiscard]] inline std::span<const std::byte>
content_key_bytes(std::span<const std::byte> file, const KelfLayout& layout) noexcept
{
    if (!layout.ok || layout.content_key_offset > file.size() ||
        kKelfContentKeyBytes > file.size() - layout.content_key_offset) {
        return {};
    }
    return file.subspan(layout.content_key_offset, kKelfContentKeyBytes);
}

} // namespace ps2hdd::magicgate
