#include "ps2hdd/fhdb_rescue.hpp"
#include "ps2hdd/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint32_t load_u32(const std::byte* p)
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

void store_u16(std::byte* p, std::uint16_t value)
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void store_u32(std::byte* p, std::uint32_t value)
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

std::uint32_t apa_checksum(std::span<const std::byte, 1024> header)
{
    std::uint32_t sum = 0;
    for (std::size_t word = 1; word < 256; ++word) {
        sum += load_u32(header.data() + static_cast<std::ptrdiff_t>(word * 4));
    }
    return sum;
}

std::array<std::byte, 1024> make_standard_apa_header()
{
    std::array<std::byte, 1024> header{};
    std::memcpy(header.data() + 0x004, "APA\0", 4);
    std::memcpy(header.data() + 0x010, "__mbr", 5);
    std::memcpy(header.data() + 0x100, "Sony Computer Entertainment Inc.", 32);
    store_u32(header.data() + 0x130, 0x2000U);
    store_u32(header.data() + 0x134, 1U);
    store_u32(header.data(), apa_checksum(header));
    return header;
}

std::vector<std::byte> make_sector_padded_kelf()
{
    std::vector<std::byte> payload(512, std::byte{0});
    // Mirrors fhdb-bootstrap-manager kelf.c: low-layout flags add 8 bytes,
    // followed by the required 32-byte key/check area.
    constexpr std::uint16_t header_size = 72;
    constexpr std::uint32_t elf_size = 8;
    store_u32(payload.data() + 0x10, elf_size);
    store_u16(payload.data() + 0x14, header_size);
    store_u16(payload.data() + 0x18, 0);
    store_u16(payload.data() + 0x1a, 0);
    for (std::size_t i = header_size; i < header_size + elf_size; ++i) {
        payload[i] = static_cast<std::byte>(0x40U + static_cast<unsigned>(i - header_size));
    }
    return payload;
}

void test_sha256_vectors_from_bootstrap_manager()
{
    const std::span<const std::byte> empty;
    check(ps2hdd::crypto::sha256_hex(ps2hdd::crypto::sha256(empty)) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "empty SHA-256 vector mismatch");

    const std::string abc = "abc";
    check(ps2hdd::crypto::sha256_hex(ps2hdd::crypto::sha256(std::as_bytes(std::span{abc}))) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "abc SHA-256 vector mismatch");

    std::array<std::byte, 128> block{};
    block.fill(std::byte{'a'});
    check(ps2hdd::crypto::sha256_hex(ps2hdd::crypto::sha256(std::span<const std::byte>(block).first(64))) ==
              "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
          "64-byte SHA-256 vector mismatch");
}

void test_exact_v1_metadata_layout()
{
    ps2hdd::fhdb::RescueCapsuleInfo info;
    info.flags = ps2hdd::fhdb::kRescueFlagValidApa |
                 ps2hdd::fhdb::kRescueFlagHasPayload |
                 ps2hdd::fhdb::kRescueFlagValidKelf;
    info.payload_start = 0x2000;
    info.payload_sectors = 4;
    info.payload_bytes = 2048;
    info.kelf_file_bytes = 1800;
    info.romver = "0220JC20060905";
    info.family = "PSBBN / OSDMenu MBR";
    info.confidence = "high";
    info.apa_sha256.fill(std::byte{0x11});
    info.payload_sha256.fill(std::byte{0x22});

    const auto metadata = ps2hdd::fhdb::encode_rescue_metadata(info);
    check(std::memcmp(metadata.data(), "PS2HBRC\0", 8) == 0, "rescue magic mismatch");
    check(load_u32(metadata.data() + 0x008) == 1, "rescue version offset mismatch");
    check(load_u32(metadata.data() + 0x00c) == 256, "metadata size offset mismatch");
    check(load_u32(metadata.data() + 0x010) == 3328, "complete size offset mismatch");
    check(load_u32(metadata.data() + 0x014) == 7, "flags offset mismatch");
    check(load_u32(metadata.data() + 0x018) == 0x2000, "payload start offset mismatch");
    check(load_u32(metadata.data() + 0x01c) == 4, "payload sector offset mismatch");
    check(load_u32(metadata.data() + 0x020) == 2048, "payload byte offset mismatch");
    check(load_u32(metadata.data() + 0x024) == 1024, "APA byte offset mismatch");
    check(load_u32(metadata.data() + 0x0a8) == 1800, "KELF length offset mismatch");
    for (std::size_t i = 0x0ac; i < metadata.size(); ++i) {
        check(metadata[i] == std::byte{0}, "v1 reserved metadata must stay zero");
    }

    const auto decoded = ps2hdd::fhdb::decode_rescue_metadata(metadata, 3328);
    check(decoded.ok, "canonical metadata should decode");
    check(decoded.info.flags == info.flags && decoded.info.payload_start == info.payload_start &&
              decoded.info.payload_sectors == info.payload_sectors &&
              decoded.info.payload_bytes == info.payload_bytes &&
              decoded.info.kelf_file_bytes == info.kelf_file_bytes,
          "canonical metadata round-trip mismatch");
    check(decoded.info.family == info.family && decoded.info.confidence == info.confidence,
          "canonical diagnostic strings did not round-trip");
}

void test_same_disk_identity_policy()
{
    auto current = make_standard_apa_header();
    auto saved = current;
    saved[0] ^= std::byte{0xff};
    store_u32(saved.data() + 0x130, 0x4000);
    store_u32(saved.data() + 0x134, 0x80);
    check(ps2hdd::fhdb::same_disk_identity(current, saved),
          "checksum and OSD pointer must be mutable identity fields");
    saved[0x200] ^= std::byte{1};
    check(!ps2hdd::fhdb::same_disk_identity(current, saved),
          "other APA bytes must participate in disk identity");
}

void test_complete_capsule_interop_and_kelf_validation()
{
    auto header = make_standard_apa_header();
    const auto payload = make_sector_padded_kelf();
    const auto built = ps2hdd::fhdb::build_rescue_image(
        header, payload, 0x2000, 1, "0220JC20060905", "FHDB / custom", "high");
    check(built.ok, "canonical rescue builder should accept standard master and payload");
    check((built.info.flags & ps2hdd::fhdb::kRescueFlagValidKelf) != 0,
          "structural KELF should set VALID_KELF");
    check(built.info.kelf_file_bytes == 80, "unpadded KELF length mismatch");

    const auto bytes = ps2hdd::fhdb::serialize_rescue_image(built);
    check(bytes.size() == 256 + 1024 + 512, "canonical rescue complete size mismatch");
    const auto parsed = ps2hdd::fhdb::validate_rescue_image(bytes);
    check(parsed.ok, "DriveForge must accept its FHDB-compatible rescue image");
    check(parsed.info.apa_sha256 == built.info.apa_sha256 &&
              parsed.info.payload_sha256 == built.info.payload_sha256,
          "rescue SHA-256 values did not round-trip");

    auto damaged = bytes;
    damaged.back() ^= std::byte{1};
    check(!ps2hdd::fhdb::validate_rescue_image(damaged).ok,
          "payload corruption must be rejected by canonical SHA-256 validation");
}

void test_canonical_rejections()
{
    ps2hdd::fhdb::RescueCapsuleInfo info;
    info.flags = ps2hdd::fhdb::kRescueFlagValidApa;
    auto metadata = ps2hdd::fhdb::encode_rescue_metadata(info);
    check(!ps2hdd::fhdb::decode_rescue_metadata(metadata, 1279).ok,
          "wrong complete size must be rejected");

    info.flags = ps2hdd::fhdb::kRescueFlagValidApa | ps2hdd::fhdb::kRescueFlagValidKelf;
    metadata = ps2hdd::fhdb::encode_rescue_metadata(info);
    check(!ps2hdd::fhdb::decode_rescue_metadata(metadata, 1280).ok,
          "VALID_KELF without payload must be rejected");

    info.flags = ps2hdd::fhdb::kRescueFlagValidApa;
    metadata = ps2hdd::fhdb::encode_rescue_metadata(info);
    metadata[0x017] = std::byte{0x80};
    check(!ps2hdd::fhdb::decode_rescue_metadata(metadata, 1280).ok,
          "unknown rescue flag bits must be rejected");
}

} // namespace

int main()
{
    try {
        test_sha256_vectors_from_bootstrap_manager();
        test_exact_v1_metadata_layout();
        test_same_disk_identity_policy();
        test_complete_capsule_interop_and_kelf_validation();
        test_canonical_rejections();
        std::cout << "FHDB rescue interoperability tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FHDB rescue interoperability tests failed: " << error.what() << '\n';
        return 1;
    }
}
