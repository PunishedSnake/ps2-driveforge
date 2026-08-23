#pragma once

#include "ps2hdd/magicgate_content.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace ps2hdd::magicgate::known_vectors {

// These values were generated with an independent standards-based DES/3DES
// implementation, not with DriveForge's cipher code. They intentionally use a
// synthetic keyset: CI gets deterministic interoperability checks without any
// Sony key material entering the repository or release binaries.

[[nodiscard]] constexpr SigningKeyset synthetic_signing_keyset() noexcept
{
    SigningKeyset keyset;
    keyset.signature_master = {
        std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
        std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef}};
    keyset.signature_hash = {
        std::byte{0xfe}, std::byte{0xdc}, std::byte{0xba}, std::byte{0x98},
        std::byte{0x76}, std::byte{0x54}, std::byte{0x32}, std::byte{0x10}};
    keyset.root_signature_master = {
        std::byte{0x13}, std::byte{0x34}, std::byte{0x57}, std::byte{0x79},
        std::byte{0x9b}, std::byte{0xbc}, std::byte{0xdf}, std::byte{0xf1}};
    keyset.root_signature_hash = {
        std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
        std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef},
        std::byte{0x23}, std::byte{0x45}, std::byte{0x67}, std::byte{0x89},
        std::byte{0xab}, std::byte{0xcd}, std::byte{0xef}, std::byte{0x01}};
    keyset.content_table_iv = {
        std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
        std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}};
    keyset.content_iv = {
        std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
        std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}};
    return keyset;
}

[[nodiscard]] constexpr DiskKeyset synthetic_disk_keyset() noexcept
{
    DiskKeyset keyset;
    keyset.kbit_master = {
        std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
        std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
        std::byte{0x88}, std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb},
        std::byte{0xcc}, std::byte{0xdd}, std::byte{0xee}, std::byte{0xff}};
    keyset.kbit_material = {
        std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
        std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}};
    keyset.kc_master = {
        std::byte{0xff}, std::byte{0xee}, std::byte{0xdd}, std::byte{0xcc},
        std::byte{0xbb}, std::byte{0xaa}, std::byte{0x99}, std::byte{0x88},
        std::byte{0x77}, std::byte{0x66}, std::byte{0x55}, std::byte{0x44},
        std::byte{0x33}, std::byte{0x22}, std::byte{0x11}, std::byte{0x00}};
    keyset.kc_material = {
        std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
        std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}};
    return keyset;
}

[[nodiscard]] constexpr KelfHeader synthetic_header() noexcept
{
    KelfHeader header;
    for (std::size_t i = 0; i < header.user_header.size(); ++i) {
        header.user_header[i] = static_cast<std::byte>(0x10U + i);
    }
    header.elf_size = 0x12345678U;
    header.header_size = 0x0090U;
    header.unknown5 = 0x0701U;
    header.flags = 0x022cU;
    header.bit_count = 0;
    header.mg_zones = 1;
    return header;
}

inline constexpr auto kSigningKeyset = synthetic_signing_keyset();
inline constexpr auto kDiskKeyset = synthetic_disk_keyset();
inline constexpr MagicGateKeyset kSyntheticKeyset{kDiskKeyset, kSigningKeyset};
inline constexpr auto kHeader = synthetic_header();

inline constexpr cipher::Block kExpectedHeaderSignature{
    std::byte{0x54}, std::byte{0x0f}, std::byte{0xb0}, std::byte{0x0a},
    std::byte{0x19}, std::byte{0x44}, std::byte{0x27}, std::byte{0x0b}};

inline constexpr std::array<std::byte, 40> kPlainBitTable{
    std::byte{0x90}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x20}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
    std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77},
    std::byte{0x40}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    std::byte{0x88}, std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb},
    std::byte{0xcc}, std::byte{0xdd}, std::byte{0xee}, std::byte{0xff}};

inline constexpr std::array<std::byte, 16> kSyntheticKbit{
    std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43},
    std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47},
    std::byte{0x48}, std::byte{0x49}, std::byte{0x4a}, std::byte{0x4b},
    std::byte{0x4c}, std::byte{0x4d}, std::byte{0x4e}, std::byte{0x4f}};
inline constexpr std::array<std::byte, 16> kSyntheticKc{
    std::byte{0xa0}, std::byte{0xa1}, std::byte{0xa2}, std::byte{0xa3},
    std::byte{0xa4}, std::byte{0xa5}, std::byte{0xa6}, std::byte{0xa7},
    std::byte{0xa8}, std::byte{0xa9}, std::byte{0xaa}, std::byte{0xab},
    std::byte{0xac}, std::byte{0xad}, std::byte{0xae}, std::byte{0xaf}};

inline constexpr std::array<std::byte, 40> kExpectedEncryptedBitTable{
    std::byte{0xec}, std::byte{0xfb}, std::byte{0x4a}, std::byte{0x01},
    std::byte{0x94}, std::byte{0x50}, std::byte{0x8e}, std::byte{0x4c},
    std::byte{0x93}, std::byte{0xe2}, std::byte{0x25}, std::byte{0xe9},
    std::byte{0x47}, std::byte{0xaf}, std::byte{0xb1}, std::byte{0x06},
    std::byte{0x3d}, std::byte{0xc0}, std::byte{0xdf}, std::byte{0x57},
    std::byte{0xb4}, std::byte{0x8f}, std::byte{0x02}, std::byte{0xee},
    std::byte{0xfe}, std::byte{0x3b}, std::byte{0x43}, std::byte{0x40},
    std::byte{0x41}, std::byte{0x6e}, std::byte{0x09}, std::byte{0xd8},
    std::byte{0x93}, std::byte{0x43}, std::byte{0x26}, std::byte{0x03},
    std::byte{0xc1}, std::byte{0x35}, std::byte{0x8d}, std::byte{0x68}};

[[nodiscard]] constexpr std::array<std::byte, 40> encrypted_bit_table_vector() noexcept
{
    auto bytes = kPlainBitTable;
    const bool ok = encrypt_plaintext_bit_table_in_place(bytes, kSyntheticKbit, kSigningKeyset);
    return ok ? bytes : std::array<std::byte, 40>{};
}

inline constexpr auto kEncryptedBitTable = encrypted_bit_table_vector();
static_assert(kEncryptedBitTable == kExpectedEncryptedBitTable);

inline constexpr cipher::Block kExpectedBitTableSignature{
    std::byte{0xf7}, std::byte{0x8e}, std::byte{0x13}, std::byte{0x62},
    std::byte{0x02}, std::byte{0x3e}, std::byte{0xef}, std::byte{0xcd}};

inline constexpr std::array<cipher::Block, 2> kSignedBlockSignatures{
    cipher::Block{
        std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33},
        std::byte{0x44}, std::byte{0x55}, std::byte{0x66}, std::byte{0x77}},
    cipher::Block{
        std::byte{0x88}, std::byte{0x99}, std::byte{0xaa}, std::byte{0xbb},
        std::byte{0xcc}, std::byte{0xdd}, std::byte{0xee}, std::byte{0xff}}};
inline constexpr cipher::Block kExpectedRootSignature{
    std::byte{0xc2}, std::byte{0x55}, std::byte{0xcc}, std::byte{0x4a},
    std::byte{0x4d}, std::byte{0xd5}, std::byte{0x86}, std::byte{0x59}};

inline constexpr std::array<std::byte, 32> kPlainContent{
    std::byte{0x30}, std::byte{0x31}, std::byte{0x32}, std::byte{0x33},
    std::byte{0x34}, std::byte{0x35}, std::byte{0x36}, std::byte{0x37},
    std::byte{0x38}, std::byte{0x39}, std::byte{0x3a}, std::byte{0x3b},
    std::byte{0x3c}, std::byte{0x3d}, std::byte{0x3e}, std::byte{0x3f},
    std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43},
    std::byte{0x44}, std::byte{0x45}, std::byte{0x46}, std::byte{0x47},
    std::byte{0x48}, std::byte{0x49}, std::byte{0x4a}, std::byte{0x4b},
    std::byte{0x4c}, std::byte{0x4d}, std::byte{0x4e}, std::byte{0x4f}};
inline constexpr cipher::Block kExpectedEncryptedStyleContentSignature{
    std::byte{0x08}, std::byte{0xd7}, std::byte{0xb4}, std::byte{0xfb},
    std::byte{0x62}, std::byte{0x9d}, std::byte{0x08}, std::byte{0x85}};
inline constexpr cipher::Block kExpectedPlainStyleContentSignature{
    std::byte{0xbe}, std::byte{0x16}, std::byte{0x82}, std::byte{0xe3},
    std::byte{0xbe}, std::byte{0x22}, std::byte{0xb6}, std::byte{0xca}};

inline constexpr std::string_view kSyntheticKeysetText =
    "# DriveForge synthetic MagicGate regression keyset\n"
    " MG_SIG_MASTER_KEY = 0123456789ABCDEF \n"
    "MG_SIG_HASH_KEY=fedcba9876543210\n"
    "MG_KBIT_MASTER_KEY=00112233445566778899aabbccddeeff\n"
    "MG_KBIT_MATERIAL = 1011121314151617\n"
    "MG_KC_MASTER_KEY=ffeeddccbbaa99887766554433221100\n"
    "MG_KC_IV=2021222324252627\n"
    "; comment between key classes\n"
    "MG_ROOTSIG_MASTER_KEY=133457799bbcdff1\n"
    "MG_ROOTSIG_HASH_KEY=0123456789abcdef23456789abcdef01\n"
    "MG_CONTENT_TABLE_IV=1011121314151617\n"
    "MG_CONTENT_IV=2021222324252627\n";

// SignatureResult and KeysetParseResult intentionally contain diagnostic
// strings. Their known-answer comparisons therefore live in runtime tests
// instead of forcing library-specific constexpr std::string behavior. The
// expected byte arrays above remain independent immutable vectors.

} // namespace ps2hdd::magicgate::known_vectors
