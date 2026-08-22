#pragma once

#include "ps2hdd/apa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::rescue {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kMetadataSize = 256;
inline constexpr std::size_t kApaHeaderSize = apa::kHeaderSize;
inline constexpr std::size_t kPayloadOffset = kMetadataSize + kApaHeaderSize;
inline constexpr std::size_t kRomverSize = 16;
inline constexpr std::size_t kFamilySize = 32;
inline constexpr std::size_t kConfidenceSize = 16;

inline constexpr std::uint32_t kFlagValidApa = 0x00000001U;
inline constexpr std::uint32_t kFlagHasPayload = 0x00000002U;
inline constexpr std::uint32_t kFlagValidKelf = 0x00000004U;
inline constexpr std::uint32_t kKnownFlags =
    kFlagValidApa | kFlagHasPayload | kFlagValidKelf;

using Sha256Digest = std::array<std::byte, 32>;

struct CapsuleInfo {
    std::uint32_t flags{};
    std::uint32_t payload_start{};
    std::uint32_t payload_sectors{};
    std::uint32_t payload_bytes{};
    std::uint32_t kelf_file_bytes{};
    Sha256Digest apa_sha256{};
    Sha256Digest payload_sha256{};
    std::string romver;
    std::string family;
    std::string confidence;

    [[nodiscard]] bool valid_apa_when_saved() const noexcept
    {
        return (flags & kFlagValidApa) != 0;
    }
    [[nodiscard]] bool has_payload() const noexcept
    {
        return (flags & kFlagHasPayload) != 0;
    }
    [[nodiscard]] bool valid_kelf_when_saved() const noexcept
    {
        return (flags & kFlagValidKelf) != 0;
    }
};

struct Capsule {
    CapsuleInfo info;
    apa::Header saved_mbr{};
    std::vector<std::byte> payload;
};

enum class CapsuleError {
    none,
    file_too_small,
    bad_magic,
    unsupported_version,
    bad_metadata_size,
    bad_apa_header_size,
    payload_size_overflow,
    payload_size_mismatch,
    complete_size_mismatch,
    unknown_flags,
    kelf_size_out_of_range,
    payload_fields_without_payload,
    payload_flag_without_range,
    kelf_flag_without_payload,
    kelf_length_without_valid_flag,
    apa_hash_mismatch,
    payload_hash_mismatch,
    invalid_embedded_apa,
};

struct ParseResult {
    std::optional<Capsule> capsule;
    CapsuleError error{CapsuleError::none};
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return capsule.has_value(); }
};

// Parse and strictly validate an HDDRESCUE.BIN / HDDRESCUE2.BIN v1 capsule.
// The metadata relationships, exact file length, embedded APA header, and
// SHA-256 digests are all checked before a Capsule is returned.
[[nodiscard]] ParseResult parse_capsule(std::span<const std::byte> file_bytes);

// The Rescue Capsule identity rule from fhdb-bootstrap-manager: checksum and
// __mbr.mbr.osd_start/osd_size are mutable; every other byte belongs to the
// disk identity comparison.
[[nodiscard]] bool same_disk_identity(const apa::Header& current,
                                      const apa::Header& saved) noexcept;

// Public because later 0.7 imaging/forensics work needs the same digest
// primitive for source/image verification and forensic reports.
[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::string sha256_hex(const Sha256Digest& digest);

} // namespace ps2hdd::rescue
