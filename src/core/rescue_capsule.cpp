#include "ps2hdd/rescue_capsule.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <string_view>

namespace ps2hdd::rescue {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'P'}, std::byte{'S'}, std::byte{'2'}, std::byte{'H'},
    std::byte{'B'}, std::byte{'R'}, std::byte{'C'}, std::byte{0},
};

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

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

std::uint32_t read_le32(std::span<const std::byte> bytes, std::size_t offset) noexcept
{
    const auto* p = bytes.data() + offset;
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

std::uint32_t read_be32(const unsigned char* source) noexcept
{
    return (static_cast<std::uint32_t>(source[0]) << 24U) |
           (static_cast<std::uint32_t>(source[1]) << 16U) |
           (static_cast<std::uint32_t>(source[2]) << 8U) |
           static_cast<std::uint32_t>(source[3]);
}

void write_be32(std::byte* destination, std::uint32_t value) noexcept
{
    destination[0] = static_cast<std::byte>(value >> 24U);
    destination[1] = static_cast<std::byte>(value >> 16U);
    destination[2] = static_cast<std::byte>(value >> 8U);
    destination[3] = static_cast<std::byte>(value);
}

struct Sha256Context {
    std::array<std::uint32_t, 8> state{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::array<unsigned char, 64> block{};
    std::uint64_t total_bytes{};
    std::size_t block_used{};
};

void sha256_transform(Sha256Context& context, const unsigned char block[64]) noexcept
{
    std::array<std::uint32_t, 16> schedule{};
    auto a = context.state[0];
    auto b = context.state[1];
    auto c = context.state[2];
    auto d = context.state[3];
    auto e = context.state[4];
    auto f = context.state[5];
    auto g = context.state[6];
    auto h = context.state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        std::uint32_t word{};
        if (i < 16) {
            word = read_be32(block + i * 4);
            schedule[i] = word;
        } else {
            const auto w15 = schedule[(i - 15) & 15U];
            const auto w2 = schedule[(i - 2) & 15U];
            const auto s0 = std::rotr(w15, 7) ^ std::rotr(w15, 18) ^ (w15 >> 3U);
            const auto s1 = std::rotr(w2, 17) ^ std::rotr(w2, 19) ^ (w2 >> 10U);
            word = schedule[i & 15U] + s0 + schedule[(i - 7) & 15U] + s1;
            schedule[i & 15U] = word;
        }

        const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choose = (e & f) ^ ((~e) & g);
        const auto temporary1 = h + sum1 + choose + kRoundConstants[i] + word;
        const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
    context.state[4] += e;
    context.state[5] += f;
    context.state[6] += g;
    context.state[7] += h;
}

void sha256_update(Sha256Context& context, const unsigned char* source, std::size_t size) noexcept
{
    context.total_bytes += size;

    if (context.block_used != 0 && size != 0) {
        const auto available = context.block.size() - context.block_used;
        const auto chunk = std::min(size, available);
        std::memcpy(context.block.data() + context.block_used, source, chunk);
        context.block_used += chunk;
        source += chunk;
        size -= chunk;
        if (context.block_used == context.block.size()) {
            sha256_transform(context, context.block.data());
            context.block_used = 0;
        }
    }

    while (context.block_used == 0 && size >= context.block.size()) {
        sha256_transform(context, source);
        source += context.block.size();
        size -= context.block.size();
    }

    if (size != 0) {
        std::memcpy(context.block.data(), source, size);
        context.block_used = size;
    }
}

Sha256Digest sha256_final(Sha256Context& context) noexcept
{
    const std::uint64_t total_bits = context.total_bytes * 8U;
    context.block[context.block_used++] = 0x80U;
    if (context.block_used > 56) {
        std::fill(context.block.begin() + static_cast<std::ptrdiff_t>(context.block_used),
                  context.block.end(), 0U);
        sha256_transform(context, context.block.data());
        context.block_used = 0;
    }
    std::fill(context.block.begin() + static_cast<std::ptrdiff_t>(context.block_used),
              context.block.begin() + 56, 0U);
    for (std::size_t i = 0; i < 8; ++i) {
        context.block[63 - i] = static_cast<unsigned char>(total_bits >> (i * 8U));
    }
    sha256_transform(context, context.block.data());

    Sha256Digest digest{};
    for (std::size_t i = 0; i < context.state.size(); ++i) {
        write_be32(digest.data() + i * 4, context.state[i]);
    }
    return digest;
}

std::string fixed_string(std::span<const std::byte> metadata,
                         std::size_t offset,
                         std::size_t width)
{
    // fhdb-bootstrap-manager copies the fixed field and forces its final byte
    // to NUL. Mirror that observable decode behavior exactly.
    std::size_t length = 0;
    while (length + 1 < width && metadata[offset + length] != std::byte{0}) {
        ++length;
    }
    return std::string(reinterpret_cast<const char*>(metadata.data() + offset), length);
}

Sha256Digest copy_digest(std::span<const std::byte> metadata, std::size_t offset) noexcept
{
    Sha256Digest digest{};
    std::copy_n(metadata.begin() + static_cast<std::ptrdiff_t>(offset),
                digest.size(), digest.begin());
    return digest;
}

ParseResult fail(CapsuleError error, std::string message)
{
    ParseResult result;
    result.error = error;
    result.message = std::move(message);
    return result;
}

bool is_standard_apa_mbr(const apa::Header& header) noexcept
{
    return header.magic == apa::kMagic &&
           apa::partition_id(header) == "__mbr" &&
           apa::has_sony_mbr_magic(header) &&
           apa::checksum(header) == header.checksum;
}

} // namespace

Sha256Digest sha256(std::span<const std::byte> bytes) noexcept
{
    Sha256Context context;
    sha256_update(context,
                  reinterpret_cast<const unsigned char*>(bytes.data()),
                  bytes.size());
    return sha256_final(context);
}

std::string sha256_hex(const Sha256Digest& digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output(digest.size() * 2, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const auto value = std::to_integer<unsigned char>(digest[i]);
        output[i * 2] = digits[value >> 4U];
        output[i * 2 + 1] = digits[value & 0x0fU];
    }
    return output;
}

bool same_disk_identity(const apa::Header& current, const apa::Header& saved) noexcept
{
    const auto current_bytes = std::as_bytes(std::span{&current, 1});
    const auto saved_bytes = std::as_bytes(std::span{&saved, 1});

    constexpr std::size_t checksum_end = sizeof(std::uint32_t);
    constexpr std::size_t osd_start =
        offsetof(apa::Header, mbr) + offsetof(apa::MbrData, osd_start);
    constexpr std::size_t mutable_end =
        offsetof(apa::Header, mbr) + offsetof(apa::MbrData, osd_size) + sizeof(std::uint32_t);

    return std::equal(current_bytes.begin() + static_cast<std::ptrdiff_t>(checksum_end),
                      current_bytes.begin() + static_cast<std::ptrdiff_t>(osd_start),
                      saved_bytes.begin() + static_cast<std::ptrdiff_t>(checksum_end)) &&
           std::equal(current_bytes.begin() + static_cast<std::ptrdiff_t>(mutable_end),
                      current_bytes.end(),
                      saved_bytes.begin() + static_cast<std::ptrdiff_t>(mutable_end));
}

ParseResult parse_capsule(std::span<const std::byte> file_bytes)
{
    if (file_bytes.size() < kPayloadOffset) {
        return fail(CapsuleError::file_too_small,
                    "Rescue Capsule is smaller than metadata + APA header");
    }

    const auto metadata = file_bytes.first(kMetadataSize);
    if (!std::equal(kMagic.begin(), kMagic.end(), metadata.begin())) {
        return fail(CapsuleError::bad_magic, "Rescue Capsule magic is not PS2HBRC\\0");
    }
    if (read_le32(metadata, kOffsetVersion) != kVersion) {
        return fail(CapsuleError::unsupported_version,
                    "Unsupported Rescue Capsule version");
    }
    if (read_le32(metadata, kOffsetMetadataSize) != kMetadataSize) {
        return fail(CapsuleError::bad_metadata_size,
                    "Rescue Capsule metadata size is not 256 bytes");
    }
    if (read_le32(metadata, kOffsetApaBytes) != kApaHeaderSize) {
        return fail(CapsuleError::bad_apa_header_size,
                    "Rescue Capsule APA header size is not 1024 bytes");
    }

    const auto payload_sectors = read_le32(metadata, kOffsetPayloadSectors);
    const auto payload_bytes = read_le32(metadata, kOffsetPayloadBytes);
    if (payload_sectors > std::numeric_limits<std::uint32_t>::max() / apa::kSectorSize) {
        return fail(CapsuleError::payload_size_overflow,
                    "Rescue Capsule payload sector count overflows byte size");
    }
    if (payload_bytes != payload_sectors * apa::kSectorSize) {
        return fail(CapsuleError::payload_size_mismatch,
                    "Rescue Capsule payload byte/sector sizes disagree");
    }
    if (payload_bytes > std::numeric_limits<std::uint32_t>::max() - kPayloadOffset) {
        return fail(CapsuleError::payload_size_overflow,
                    "Rescue Capsule complete size overflows v1 size field");
    }

    const auto recorded_size = read_le32(metadata, kOffsetCompleteSize);
    const auto expected_size = kPayloadOffset + static_cast<std::size_t>(payload_bytes);
    if (recorded_size != expected_size || file_bytes.size() != expected_size) {
        return fail(CapsuleError::complete_size_mismatch,
                    "Rescue Capsule recorded/actual complete size mismatch");
    }

    Capsule capsule;
    capsule.info.flags = read_le32(metadata, kOffsetFlags);
    if ((capsule.info.flags & ~kKnownFlags) != 0) {
        return fail(CapsuleError::unknown_flags,
                    "Rescue Capsule contains unknown v1 flag bits");
    }
    capsule.info.payload_start = read_le32(metadata, kOffsetPayloadStart);
    capsule.info.payload_sectors = payload_sectors;
    capsule.info.payload_bytes = payload_bytes;
    capsule.info.kelf_file_bytes = read_le32(metadata, kOffsetKelfFileBytes);

    if (capsule.info.kelf_file_bytes > capsule.info.payload_bytes) {
        return fail(CapsuleError::kelf_size_out_of_range,
                    "Rescue Capsule KELF length exceeds payload size");
    }
    if (!capsule.info.has_payload() &&
        (capsule.info.payload_start != 0 || capsule.info.payload_sectors != 0 ||
         capsule.info.payload_bytes != 0)) {
        return fail(CapsuleError::payload_fields_without_payload,
                    "Rescue Capsule has payload fields without HAS_PAYLOAD");
    }
    if (capsule.info.has_payload() &&
        (capsule.info.payload_start == 0 || capsule.info.payload_sectors == 0)) {
        return fail(CapsuleError::payload_flag_without_range,
                    "Rescue Capsule HAS_PAYLOAD flag has no payload range");
    }
    if (capsule.info.valid_kelf_when_saved() &&
        (!capsule.info.has_payload() || capsule.info.kelf_file_bytes == 0)) {
        return fail(CapsuleError::kelf_flag_without_payload,
                    "Rescue Capsule VALID_KELF requires a non-empty payload");
    }
    if (!capsule.info.valid_kelf_when_saved() && capsule.info.kelf_file_bytes != 0) {
        return fail(CapsuleError::kelf_length_without_valid_flag,
                    "Rescue Capsule KELF length is set without VALID_KELF");
    }

    capsule.info.apa_sha256 = copy_digest(metadata, kOffsetApaSha256);
    capsule.info.payload_sha256 = copy_digest(metadata, kOffsetPayloadSha256);
    capsule.info.romver = fixed_string(metadata, kOffsetRomver, kRomverSize);
    capsule.info.family = fixed_string(metadata, kOffsetFamily, kFamilySize);
    capsule.info.confidence = fixed_string(metadata, kOffsetConfidence, kConfidenceSize);

    const auto saved_header_bytes = file_bytes.subspan(kMetadataSize, kApaHeaderSize);
    if (sha256(saved_header_bytes) != capsule.info.apa_sha256) {
        return fail(CapsuleError::apa_hash_mismatch,
                    "Rescue Capsule APA SHA-256 does not match embedded header bytes");
    }

    std::memcpy(&capsule.saved_mbr, saved_header_bytes.data(), sizeof(capsule.saved_mbr));
    if (!is_standard_apa_mbr(capsule.saved_mbr)) {
        return fail(CapsuleError::invalid_embedded_apa,
                    "Rescue Capsule embedded APA __mbr header is invalid");
    }

    if (capsule.info.has_payload()) {
        const auto payload = file_bytes.subspan(kPayloadOffset, capsule.info.payload_bytes);
        if (sha256(payload) != capsule.info.payload_sha256) {
            return fail(CapsuleError::payload_hash_mismatch,
                        "Rescue Capsule payload SHA-256 does not match payload bytes");
        }
        capsule.payload.assign(payload.begin(), payload.end());
    }

    ParseResult result;
    result.capsule = std::move(capsule);
    return result;
}

} // namespace ps2hdd::rescue
