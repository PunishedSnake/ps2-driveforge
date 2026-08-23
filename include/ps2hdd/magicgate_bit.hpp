#pragma once

#include "ps2hdd/magicgate_cipher.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::magicgate {

inline constexpr std::uint32_t kBitBlockEncrypted = 0x01U;
inline constexpr std::uint32_t kBitBlockSigned = 0x02U;
inline constexpr std::size_t kBitTableFixedBytes = 8U;
inline constexpr std::size_t kBitDescriptorBytes = 16U;
inline constexpr std::size_t kBitMaxBlocks = 63U;

struct BitBlock {
    std::uint32_t size{};
    std::uint32_t flags{};
    cipher::Block signature{};
};

struct BitTable {
    std::uint32_t header_size{};
    std::uint8_t block_count{};
    std::vector<BitBlock> blocks;
    std::uint64_t payload_bytes{};
};

struct BitTableResult {
    bool ok{};
    std::string error;
    BitTable table;
    std::size_t encoded_bytes{};
};

namespace bit_detail {

[[nodiscard]] inline std::uint32_t load_le32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

} // namespace bit_detail

// Parse the BIT table only after the MagicGate header layer has produced
// plaintext. Trying to auto-detect encrypted bytes here would turn corruption
// into interpretation, which is a charming property for a file viewer and a
// terrible one for a signer.
[[nodiscard]] inline BitTableResult parse_plain_bit_table(
    std::span<const std::byte> bytes,
    std::uint16_t expected_bit_count = 0)
{
    BitTableResult result;
    if (bytes.size() < kBitTableFixedBytes) {
        result.error = "BIT table is smaller than its 8-byte header";
        return result;
    }

    auto& table = result.table;
    table.header_size = bit_detail::load_le32(bytes.data());
    table.block_count = std::to_integer<std::uint8_t>(bytes[4]);

    if (table.block_count > kBitMaxBlocks) {
        result.error = "BIT block_count exceeds SECRMAN's 63-block limit";
        return result;
    }
    if (expected_bit_count != 0 && table.block_count != expected_bit_count) {
        result.error = "BIT block_count disagrees with the KELF header";
        return result;
    }

    const auto descriptor_bytes = static_cast<std::size_t>(table.block_count) *
                                  kBitDescriptorBytes;
    if (descriptor_bytes > std::numeric_limits<std::size_t>::max() -
                               kBitTableFixedBytes) {
        result.error = "BIT table size overflow";
        return result;
    }
    result.encoded_bytes = kBitTableFixedBytes + descriptor_bytes;
    if (bytes.size() < result.encoded_bytes) {
        result.error = "BIT descriptors are truncated";
        return result;
    }

    table.blocks.reserve(table.block_count);
    std::uint64_t payload_bytes = 0;
    for (std::size_t i = 0; i < table.block_count; ++i) {
        const auto offset = kBitTableFixedBytes + i * kBitDescriptorBytes;
        BitBlock block;
        block.size = bit_detail::load_le32(bytes.data() + offset);
        block.flags = bit_detail::load_le32(bytes.data() + offset + 4U);
        for (std::size_t j = 0; j < block.signature.size(); ++j) {
            block.signature[j] = bytes[offset + 8U + j];
        }

        if ((block.flags & ~(kBitBlockEncrypted | kBitBlockSigned)) != 0) {
            result.error = "BIT block contains unsupported flag bits";
            return result;
        }
        if (block.size == 0) {
            result.error = "BIT block has zero size";
            return result;
        }

        // Every operation implemented by SECRMAN/KELF tooling works on whole
        // DES blocks. A signed or encrypted odd-sized block is therefore not a
        // clever edge case - it is a malformed request to the crypto layer.
        if ((block.flags & (kBitBlockEncrypted | kBitBlockSigned)) != 0 &&
            (block.size % cipher::Block{}.size()) != 0) {
            result.error = "signed/encrypted BIT block is not 8-byte aligned";
            return result;
        }
        if (payload_bytes > std::numeric_limits<std::uint64_t>::max() - block.size) {
            result.error = "BIT payload size overflow";
            return result;
        }
        payload_bytes += block.size;
        table.blocks.push_back(block);
    }

    table.payload_bytes = payload_bytes;
    result.ok = true;
    return result;
}

} // namespace ps2hdd::magicgate
