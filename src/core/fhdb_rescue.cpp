#include "ps2hdd/fhdb_rescue.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace ps2hdd::fhdb {
namespace {

constexpr std::size_t kOffsetMagic = 0x000;
constexpr std::size_t kOffsetVersion = 0x008;
constexpr std::size_t kOffsetMetadataSize = 0x00c;
constexpr std::size_t kOffsetCompleteSize = 0x010;
constexpr std::size_t kOffsetFlags = 0x014;
constexpr std::size_t kOffsetPayloadStart = 0x018;
constexpr std::size_t kOffsetPayloadSectors = 0x01c;
constexpr std::size_t kOffsetPayloadBytes = 0x020;
constexpr std::size_t kOffsetApaBytes = 0x024;
constexpr std::size_t kOffsetApaSha256 = 0x028;
constexpr std::size_t kOffsetPayloadSha256 = 0x048;
constexpr std::size_t kOffsetRomver = 0x068;
constexpr std::size_t kOffsetFamily = 0x078;
constexpr std::size_t kOffsetConfidence = 0x098;
constexpr std::size_t kOffsetKelfFileBytes = 0x0a8;
constexpr std::array<std::byte, 8> kMagic{
    std::byte{'P'}, std::byte{'S'}, std::byte{'2'}, std::byte{'H'},
    std::byte{'B'}, std::byte{'R'}, std::byte{'C'}, std::byte{0}};

constexpr std::size_t kApaMagicOffset = 0x004;
constexpr std::size_t kApaIdOffset = 0x010;
constexpr std::size_t kApaMbrMagicOffset = 0x100;
constexpr std::size_t kApaOsdStartOffset = 0x130;
constexpr std::size_t kApaOsdSizeOffset = 0x134;
constexpr std::size_t kPcMbrSignatureOffset = 0x1fe;

constexpr std::size_t kKelfFixedHeaderSize = 32;
constexpr std::size_t kKelfBitBlockSize = 16;
constexpr std::uint16_t kKelfMaxBitCount = 63;
constexpr std::size_t kKelfElfSizeOffset = 0x10;
constexpr std::size_t kKelfHeaderSizeOffset = 0x14;
constexpr std::size_t kKelfFlagsOffset = 0x18;
constexpr std::size_t kKelfBitCountOffset = 0x1a;
constexpr std::size_t kKelfOptionalAreaBytes = 8;
constexpr std::size_t kKelfKeyAreaBytes = 32;

[[nodiscard]] std::uint16_t load_u16(const std::byte* p) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[0])) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[1])) << 8U);
}

[[nodiscard]] std::uint32_t load_u32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

void store_u32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::uint32_t raw_apa_checksum(
    std::span<const std::byte, kRescueApaHeaderBytes> header) noexcept
{
    std::uint32_t sum = 0;
    for (std::size_t word = 1; word < 256; ++word) {
        sum += load_u32(header.data() + static_cast<std::ptrdiff_t>(word * 4));
    }
    return sum;
}

void store_text(std::span<std::byte> out, std::string_view text) noexcept
{
    if (out.empty()) {
        return;
    }
    const auto count = std::min(text.size(), out.size() - 1U);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
}

[[nodiscard]] std::string load_text(std::span<const std::byte> bytes)
{
    std::string value;
    value.reserve(bytes.size());
    for (const auto byte : bytes) {
        const auto ch = std::to_integer<unsigned char>(byte);
        if (ch == 0) {
            break;
        }
        value.push_back(static_cast<char>(ch));
    }
    return value;
}

[[nodiscard]] bool kelf_validate_layout(std::span<const std::byte> data) noexcept
{
    if (data.size() < kKelfFixedHeaderSize) {
        return false;
    }
    if (std::to_integer<unsigned char>(data[0]) == 0x7fU &&
        data[1] == std::byte{'E'} && data[2] == std::byte{'L'} && data[3] == std::byte{'F'}) {
        return false;
    }

    const auto elf_size = load_u32(data.data() + kKelfElfSizeOffset);
    const auto header_size = load_u16(data.data() + kKelfHeaderSizeOffset);
    const auto flags = load_u16(data.data() + kKelfFlagsOffset);
    const auto bit_count = load_u16(data.data() + kKelfBitCountOffset);
    if (header_size < kKelfFixedHeaderSize || header_size > data.size() ||
        bit_count > kKelfMaxBitCount) {
        return false;
    }
    if (elf_size == 0 || elf_size > data.size() - header_size ||
        static_cast<std::size_t>(header_size) + elf_size != data.size()) {
        return false;
    }

    std::size_t offset = kKelfFixedHeaderSize +
                         static_cast<std::size_t>(bit_count) * kKelfBitBlockSize;
    if (offset > header_size) {
        return false;
    }
    if ((flags & 1U) != 0) {
        if (offset >= header_size) {
            return false;
        }
        offset += std::to_integer<unsigned char>(data[offset]) + 1U;
    }
    if ((flags & 0xf000U) == 0) {
        offset += kKelfOptionalAreaBytes;
    }
    return offset <= header_size && kKelfKeyAreaBytes <= header_size - offset;
}

[[nodiscard]] bool kelf_size_from_disk_image(std::span<const std::byte> data,
                                             std::uint32_t& file_bytes) noexcept
{
    if (data.size() < kKelfFixedHeaderSize) {
        return false;
    }
    const auto elf_size = load_u32(data.data() + kKelfElfSizeOffset);
    const auto header_size = load_u16(data.data() + kKelfHeaderSizeOffset);
    if (elf_size > data.size() || header_size > data.size() - elf_size) {
        return false;
    }
    const auto calculated64 = static_cast<std::uint64_t>(header_size) + elf_size;
    if (calculated64 == 0 || calculated64 > data.size() ||
        calculated64 > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const auto calculated = static_cast<std::size_t>(calculated64);
    if (!kelf_validate_layout(data.first(calculated))) {
        return false;
    }
    file_bytes = static_cast<std::uint32_t>(calculated);
    return true;
}

} // namespace

std::array<std::byte, kRescueMetadataBytes>
encode_rescue_metadata(const RescueCapsuleInfo& info)
{
    std::array<std::byte, kRescueMetadataBytes> metadata{};
    std::copy(kMagic.begin(), kMagic.end(), metadata.begin() + kOffsetMagic);
    store_u32(metadata.data() + kOffsetVersion, kRescueCapsuleVersion);
    store_u32(metadata.data() + kOffsetMetadataSize,
              static_cast<std::uint32_t>(kRescueMetadataBytes));
    store_u32(metadata.data() + kOffsetCompleteSize,
              static_cast<std::uint32_t>(kRescueMetadataBytes + kRescueApaHeaderBytes) +
                  info.payload_bytes);
    store_u32(metadata.data() + kOffsetFlags, info.flags);
    store_u32(metadata.data() + kOffsetPayloadStart, info.payload_start);
    store_u32(metadata.data() + kOffsetPayloadSectors, info.payload_sectors);
    store_u32(metadata.data() + kOffsetPayloadBytes, info.payload_bytes);
    store_u32(metadata.data() + kOffsetApaBytes,
              static_cast<std::uint32_t>(kRescueApaHeaderBytes));
    std::copy(info.apa_sha256.begin(), info.apa_sha256.end(),
              metadata.begin() + kOffsetApaSha256);
    std::copy(info.payload_sha256.begin(), info.payload_sha256.end(),
              metadata.begin() + kOffsetPayloadSha256);
    store_text(std::span<std::byte>(metadata).subspan(kOffsetRomver, kRescueRomverBytes),
               info.romver);
    store_text(std::span<std::byte>(metadata).subspan(kOffsetFamily, kRescueFamilyBytes),
               info.family);
    store_text(std::span<std::byte>(metadata).subspan(kOffsetConfidence, kRescueConfidenceBytes),
               info.confidence);
    store_u32(metadata.data() + kOffsetKelfFileBytes, info.kelf_file_bytes);
    return metadata;
}

RescueDecodeResult decode_rescue_metadata(
    std::span<const std::byte, kRescueMetadataBytes> metadata,
    std::size_t complete_file_size)
{
    RescueDecodeResult result;
    if (!std::equal(kMagic.begin(), kMagic.end(), metadata.begin() + kOffsetMagic)) {
        result.error = "FHDB rescue capsule magic is invalid";
        return result;
    }
    if (load_u32(metadata.data() + kOffsetVersion) != kRescueCapsuleVersion) {
        result.error = "FHDB rescue capsule version is unsupported";
        return result;
    }
    if (load_u32(metadata.data() + kOffsetMetadataSize) != kRescueMetadataBytes ||
        load_u32(metadata.data() + kOffsetApaBytes) != kRescueApaHeaderBytes) {
        result.error = "FHDB rescue capsule fixed sizes are invalid";
        return result;
    }

    const auto payload_bytes = load_u32(metadata.data() + kOffsetPayloadBytes);
    const auto payload_sectors = load_u32(metadata.data() + kOffsetPayloadSectors);
    if (payload_sectors > std::numeric_limits<std::uint32_t>::max() / 512U ||
        payload_bytes != payload_sectors * 512U) {
        result.error = "FHDB rescue capsule payload size relationship is invalid";
        return result;
    }
    const auto expected = static_cast<std::uint64_t>(kRescueMetadataBytes) +
                          kRescueApaHeaderBytes + payload_bytes;
    if (expected > std::numeric_limits<std::uint32_t>::max() ||
        load_u32(metadata.data() + kOffsetCompleteSize) != expected ||
        complete_file_size != expected) {
        result.error = "FHDB rescue capsule complete file size does not match metadata";
        return result;
    }

    auto& info = result.info;
    info.flags = load_u32(metadata.data() + kOffsetFlags);
    constexpr auto known_flags = kRescueFlagValidApa | kRescueFlagHasPayload |
                                 kRescueFlagValidKelf;
    if ((info.flags & ~known_flags) != 0) {
        result.error = "FHDB rescue capsule contains unknown flags";
        return result;
    }
    info.payload_start = load_u32(metadata.data() + kOffsetPayloadStart);
    info.payload_sectors = payload_sectors;
    info.payload_bytes = payload_bytes;
    info.kelf_file_bytes = load_u32(metadata.data() + kOffsetKelfFileBytes);
    if (info.kelf_file_bytes > info.payload_bytes) {
        result.error = "FHDB rescue capsule KELF length exceeds payload";
        return result;
    }
    if ((info.flags & kRescueFlagHasPayload) == 0) {
        if (info.payload_start != 0 || info.payload_sectors != 0 || info.payload_bytes != 0) {
            result.error = "FHDB rescue capsule has payload geometry without HAS_PAYLOAD";
            return result;
        }
    } else if (info.payload_start == 0 || info.payload_sectors == 0) {
        result.error = "FHDB rescue capsule HAS_PAYLOAD has zero geometry";
        return result;
    }
    if ((info.flags & kRescueFlagValidKelf) != 0) {
        if ((info.flags & kRescueFlagHasPayload) == 0 || info.kelf_file_bytes == 0) {
            result.error = "FHDB rescue capsule VALID_KELF is inconsistent";
            return result;
        }
    } else if (info.kelf_file_bytes != 0) {
        result.error = "FHDB rescue capsule records KELF bytes without VALID_KELF";
        return result;
    }

    std::copy_n(metadata.begin() + kOffsetApaSha256, 32, info.apa_sha256.begin());
    std::copy_n(metadata.begin() + kOffsetPayloadSha256, 32, info.payload_sha256.begin());
    info.romver = load_text(metadata.subspan(kOffsetRomver, kRescueRomverBytes));
    info.family = load_text(metadata.subspan(kOffsetFamily, kRescueFamilyBytes));
    info.confidence = load_text(metadata.subspan(kOffsetConfidence, kRescueConfidenceBytes));
    result.ok = true;
    return result;
}

bool is_standard_apa_master(
    std::span<const std::byte, kRescueApaHeaderBytes> header) noexcept
{
    constexpr std::array<std::byte, 4> apa_magic{
        std::byte{'A'}, std::byte{'P'}, std::byte{'A'}, std::byte{0}};
    constexpr char mbr_id[] = "__mbr";
    constexpr char sony[] = "Sony Computer Entertainment Inc.";
    if (!std::equal(apa_magic.begin(), apa_magic.end(), header.begin() + kApaMagicOffset) ||
        std::memcmp(header.data() + kApaIdOffset, mbr_id, sizeof(mbr_id) - 1) != 0 ||
        std::memcmp(header.data() + kApaMbrMagicOffset, sony, sizeof(sony) - 1) != 0) {
        return false;
    }
    return load_u32(header.data()) == raw_apa_checksum(header);
}

bool is_hybrid_gpt_master(
    std::span<const std::byte, kRescueApaHeaderBytes> header) noexcept
{
    return header[kPcMbrSignatureOffset] == std::byte{0x55} &&
           header[kPcMbrSignatureOffset + 1] == std::byte{0xaa};
}

bool same_disk_identity(
    std::span<const std::byte, kRescueApaHeaderBytes> current,
    std::span<const std::byte, kRescueApaHeaderBytes> saved) noexcept
{
    constexpr auto mutable_end = kApaOsdSizeOffset + 4U;
    return std::equal(current.begin() + 4,
                      current.begin() + static_cast<std::ptrdiff_t>(kApaOsdStartOffset),
                      saved.begin() + 4) &&
           std::equal(current.begin() + static_cast<std::ptrdiff_t>(mutable_end), current.end(),
                      saved.begin() + static_cast<std::ptrdiff_t>(mutable_end));
}

RescueImageResult validate_rescue_image(std::span<const std::byte> image)
{
    RescueImageResult result;
    if (image.size() < kRescueMetadataBytes + kRescueApaHeaderBytes) {
        result.error = "FHDB rescue image is too small";
        return result;
    }
    std::array<std::byte, kRescueMetadataBytes> metadata{};
    std::copy_n(image.begin(), metadata.size(), metadata.begin());
    const auto decoded = decode_rescue_metadata(metadata, image.size());
    if (!decoded.ok) {
        result.error = decoded.error;
        return result;
    }
    result.info = decoded.info;
    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(kRescueMetadataBytes),
                kRescueApaHeaderBytes, result.apa_header.begin());
    if ((result.info.flags & kRescueFlagValidApa) == 0 ||
        !is_standard_apa_master(result.apa_header) ||
        crypto::sha256(result.apa_header) != result.info.apa_sha256) {
        result.error = "FHDB rescue image APA header failed structure or SHA-256 validation";
        return result;
    }

    const auto payload_begin = kRescueMetadataBytes + kRescueApaHeaderBytes;
    result.payload.assign(image.begin() + static_cast<std::ptrdiff_t>(payload_begin), image.end());
    if ((result.info.flags & kRescueFlagHasPayload) != 0) {
        if (result.payload.size() != result.info.payload_bytes ||
            crypto::sha256(result.payload) != result.info.payload_sha256) {
            result.error = "FHDB rescue image payload SHA-256 does not match";
            return result;
        }
        if ((result.info.flags & kRescueFlagValidKelf) != 0) {
            std::uint32_t kelf_bytes = 0;
            if (!kelf_size_from_disk_image(result.payload, kelf_bytes) ||
                kelf_bytes != result.info.kelf_file_bytes) {
                result.error = "FHDB rescue image KELF structure/length does not match metadata";
                return result;
            }
        }
    }
    result.ok = true;
    return result;
}

RescueImageResult build_rescue_image(
    std::span<const std::byte, kRescueApaHeaderBytes> apa_header,
    std::span<const std::byte> payload,
    std::uint32_t payload_start,
    std::uint32_t payload_sectors,
    std::string romver,
    std::string family,
    std::string confidence)
{
    RescueImageResult result;
    if (!is_standard_apa_master(apa_header)) {
        result.error = "Cannot build FHDB rescue capsule from an invalid APA master header";
        return result;
    }
    if ((payload_start == 0) != (payload_sectors == 0)) {
        result.error = "FHDB rescue payload pointer is inconsistent";
        return result;
    }
    if (payload_sectors > std::numeric_limits<std::uint32_t>::max() / 512U ||
        payload.size() != static_cast<std::uint64_t>(payload_sectors) * 512ULL) {
        result.error = "FHDB rescue payload bytes do not match sector count";
        return result;
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "FHDB rescue payload is outside the v1 size range";
        return result;
    }

    std::copy(apa_header.begin(), apa_header.end(), result.apa_header.begin());
    result.payload.assign(payload.begin(), payload.end());
    auto& info = result.info;
    info.flags = kRescueFlagValidApa;
    info.payload_start = payload_start;
    info.payload_sectors = payload_sectors;
    info.payload_bytes = static_cast<std::uint32_t>(payload.size());
    info.romver = std::move(romver);
    info.family = std::move(family);
    info.confidence = std::move(confidence);
    info.apa_sha256 = crypto::sha256(result.apa_header);

    if (!payload.empty()) {
        if (payload_start == 0 || payload_sectors == 0) {
            result.error = "FHDB rescue payload requires non-zero on-disk geometry";
            return result;
        }
        info.flags |= kRescueFlagHasPayload;
        info.payload_sha256 = crypto::sha256(result.payload);
        std::uint32_t kelf_bytes = 0;
        if (kelf_size_from_disk_image(result.payload, kelf_bytes)) {
            info.flags |= kRescueFlagValidKelf;
            info.kelf_file_bytes = kelf_bytes;
        }
    }
    result.ok = true;
    return result;
}

std::vector<std::byte> serialize_rescue_image(const RescueImageResult& image)
{
    if (!image.ok || image.info.payload_bytes != image.payload.size()) {
        return {};
    }
    const auto metadata = encode_rescue_metadata(image.info);
    std::vector<std::byte> bytes;
    bytes.reserve(metadata.size() + image.apa_header.size() + image.payload.size());
    bytes.insert(bytes.end(), metadata.begin(), metadata.end());
    bytes.insert(bytes.end(), image.apa_header.begin(), image.apa_header.end());
    bytes.insert(bytes.end(), image.payload.begin(), image.payload.end());
    return bytes;
}

} // namespace ps2hdd::fhdb
