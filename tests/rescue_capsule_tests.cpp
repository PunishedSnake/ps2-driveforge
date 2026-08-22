#include "ps2hdd/rescue_capsule.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using ps2hdd::rescue::CapsuleError;
using ps2hdd::rescue::Sha256Digest;

void write_le32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::byte>(value);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 2] = static_cast<std::byte>(value >> 16U);
    bytes[offset + 3] = static_cast<std::byte>(value >> 24U);
}

void copy_text(std::span<std::byte> bytes, std::size_t offset, std::size_t width,
               std::string_view text)
{
    const auto count = std::min(width, text.size());
    std::memcpy(bytes.data() + offset, text.data(), count);
}

ps2hdd::apa::Header make_standard_mbr()
{
    ps2hdd::apa::Header header{};
    header.magic = ps2hdd::apa::kMagic;
    std::memcpy(header.id, "__mbr", 5);
    header.start = 0;
    header.length = 0x40000;
    header.type = ps2hdd::apa::kTypeMbr;
    std::memcpy(header.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    header.mbr.version = 2;
    header.mbr.osd_start = 0x2000;
    header.mbr.osd_size = 4;
    header.checksum = ps2hdd::apa::checksum(header);
    return header;
}

std::vector<std::byte> make_capsule(bool with_payload)
{
    constexpr std::size_t metadata_size = ps2hdd::rescue::kMetadataSize;
    constexpr std::size_t apa_size = ps2hdd::rescue::kApaHeaderSize;
    constexpr std::size_t payload_size = 4 * ps2hdd::apa::kSectorSize;
    const auto total_size = metadata_size + apa_size + (with_payload ? payload_size : 0);

    std::vector<std::byte> bytes(total_size, std::byte{0});
    auto metadata = std::span<std::byte>(bytes).first(metadata_size);
    constexpr std::array<unsigned char, 8> magic{'P', 'S', '2', 'H', 'B', 'R', 'C', 0};
    for (std::size_t i = 0; i < magic.size(); ++i) {
        metadata[i] = static_cast<std::byte>(magic[i]);
    }

    write_le32(metadata, 0x008, ps2hdd::rescue::kVersion);
    write_le32(metadata, 0x00c, static_cast<std::uint32_t>(metadata_size));
    write_le32(metadata, 0x010, static_cast<std::uint32_t>(total_size));
    write_le32(metadata, 0x024, static_cast<std::uint32_t>(apa_size));

    std::uint32_t flags = ps2hdd::rescue::kFlagValidApa;
    if (with_payload) {
        flags |= ps2hdd::rescue::kFlagHasPayload | ps2hdd::rescue::kFlagValidKelf;
        write_le32(metadata, 0x018, 0x2000);
        write_le32(metadata, 0x01c, 4);
        write_le32(metadata, 0x020, static_cast<std::uint32_t>(payload_size));
        write_le32(metadata, 0x0a8, 1800);
    }
    write_le32(metadata, 0x014, flags);

    copy_text(metadata, 0x068, 16, "0220JC20060905");
    copy_text(metadata, 0x078, 32, "PSBBN / OSDMenu MBR");
    copy_text(metadata, 0x098, 16, "high");

    const auto header = make_standard_mbr();
    std::memcpy(bytes.data() + metadata_size, &header, sizeof(header));
    const auto header_span = std::span<const std::byte>(bytes).subspan(metadata_size, apa_size);
    const auto apa_digest = ps2hdd::rescue::sha256(header_span);
    std::copy(apa_digest.begin(), apa_digest.end(), bytes.begin() + 0x028);

    if (with_payload) {
        auto payload = std::span<std::byte>(bytes).subspan(metadata_size + apa_size, payload_size);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::byte>((i * 37U + 11U) & 0xffU);
        }
        const auto payload_digest = ps2hdd::rescue::sha256(payload);
        std::copy(payload_digest.begin(), payload_digest.end(), bytes.begin() + 0x048);
    }

    return bytes;
}

bool test_sha256_vectors()
{
    const std::array<std::byte, 0> empty{};
    if (ps2hdd::rescue::sha256_hex(ps2hdd::rescue::sha256(empty)) !=
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
        return false;
    }

    constexpr std::array<unsigned char, 3> abc{'a', 'b', 'c'};
    std::array<std::byte, abc.size()> bytes{};
    for (std::size_t i = 0; i < abc.size(); ++i) {
        bytes[i] = static_cast<std::byte>(abc[i]);
    }
    return ps2hdd::rescue::sha256_hex(ps2hdd::rescue::sha256(bytes)) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
}

bool test_full_capsule()
{
    auto bytes = make_capsule(true);
    auto result = ps2hdd::rescue::parse_capsule(bytes);
    if (!result.ok()) {
        std::cerr << "Full capsule parse failed: " << result.message << '\n';
        return false;
    }

    const auto& capsule = *result.capsule;
    return capsule.info.valid_apa_when_saved() &&
           capsule.info.has_payload() &&
           capsule.info.valid_kelf_when_saved() &&
           capsule.info.payload_start == 0x2000 &&
           capsule.info.payload_sectors == 4 &&
           capsule.info.payload_bytes == 2048 &&
           capsule.info.kelf_file_bytes == 1800 &&
           capsule.info.romver == "0220JC20060905" &&
           capsule.info.family == "PSBBN / OSDMenu MBR" &&
           capsule.info.confidence == "high" &&
           capsule.payload.size() == 2048 &&
           ps2hdd::apa::partition_id(capsule.saved_mbr) == "__mbr";
}

bool test_header_only_capsule()
{
    auto bytes = make_capsule(false);
    auto result = ps2hdd::rescue::parse_capsule(bytes);
    return result.ok() && result.capsule->payload.empty() &&
           !result.capsule->info.has_payload();
}

bool test_rejects_wrong_size()
{
    auto bytes = make_capsule(false);
    bytes.pop_back();
    const auto result = ps2hdd::rescue::parse_capsule(bytes);
    return !result.ok() && result.error == CapsuleError::file_too_small;
}

bool test_rejects_unknown_flags()
{
    auto bytes = make_capsule(false);
    auto metadata = std::span<std::byte>(bytes).first(ps2hdd::rescue::kMetadataSize);
    write_le32(metadata, 0x014, ps2hdd::rescue::kFlagValidApa | 0x80000000U);
    const auto result = ps2hdd::rescue::parse_capsule(bytes);
    return !result.ok() && result.error == CapsuleError::unknown_flags;
}

bool test_rejects_impossible_kelf()
{
    auto bytes = make_capsule(false);
    auto metadata = std::span<std::byte>(bytes).first(ps2hdd::rescue::kMetadataSize);
    write_le32(metadata, 0x014, ps2hdd::rescue::kFlagValidApa |
                                ps2hdd::rescue::kFlagValidKelf);
    const auto result = ps2hdd::rescue::parse_capsule(bytes);
    return !result.ok() && result.error == CapsuleError::kelf_flag_without_payload;
}

bool test_rejects_apa_hash_mismatch()
{
    auto bytes = make_capsule(false);
    bytes[ps2hdd::rescue::kMetadataSize + 0x200] ^= std::byte{1};
    const auto result = ps2hdd::rescue::parse_capsule(bytes);
    return !result.ok() && result.error == CapsuleError::apa_hash_mismatch;
}

bool test_rejects_payload_hash_mismatch()
{
    auto bytes = make_capsule(true);
    bytes.back() ^= std::byte{1};
    const auto result = ps2hdd::rescue::parse_capsule(bytes);
    return !result.ok() && result.error == CapsuleError::payload_hash_mismatch;
}

bool test_same_disk_identity()
{
    auto current = make_standard_mbr();
    auto saved = current;

    saved.checksum ^= 0xffffffffU;
    saved.mbr.osd_start = 0x4000;
    saved.mbr.osd_size = 0x80;
    if (!ps2hdd::rescue::same_disk_identity(current, saved)) {
        return false;
    }

    saved = current;
    saved.padding2[7] ^= 1;
    return !ps2hdd::rescue::same_disk_identity(current, saved);
}

} // namespace

int main()
{
    if (!test_sha256_vectors()) {
        std::cerr << "SHA-256 compatibility vectors failed.\n";
        return 1;
    }
    if (!test_full_capsule()) {
        return 2;
    }
    if (!test_header_only_capsule()) {
        std::cerr << "Header-only Rescue Capsule failed.\n";
        return 3;
    }
    if (!test_rejects_wrong_size()) {
        std::cerr << "Wrong-size Rescue Capsule was not rejected.\n";
        return 4;
    }
    if (!test_rejects_unknown_flags()) {
        std::cerr << "Unknown Rescue Capsule flags were not rejected.\n";
        return 5;
    }
    if (!test_rejects_impossible_kelf()) {
        std::cerr << "Impossible VALID_KELF state was not rejected.\n";
        return 6;
    }
    if (!test_rejects_apa_hash_mismatch()) {
        std::cerr << "APA hash mismatch was not rejected.\n";
        return 7;
    }
    if (!test_rejects_payload_hash_mismatch()) {
        std::cerr << "Payload hash mismatch was not rejected.\n";
        return 8;
    }
    if (!test_same_disk_identity()) {
        std::cerr << "Rescue Capsule same-disk identity rule failed.\n";
        return 9;
    }

    std::cout << "Rescue Capsule v1 compatibility tests passed.\n";
    return 0;
}
