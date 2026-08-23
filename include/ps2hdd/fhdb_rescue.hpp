#pragma once

#include "ps2hdd/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::fhdb {

inline constexpr std::uint32_t kRescueCapsuleVersion = 1;
inline constexpr std::size_t kRescueMetadataBytes = 256;
inline constexpr std::size_t kRescueApaHeaderBytes = 1024;
inline constexpr std::size_t kRescueFamilyBytes = 32;
inline constexpr std::size_t kRescueConfidenceBytes = 16;
inline constexpr std::size_t kRescueRomverBytes = 16;

inline constexpr std::uint32_t kRescueFlagValidApa = 0x00000001U;
inline constexpr std::uint32_t kRescueFlagHasPayload = 0x00000002U;
inline constexpr std::uint32_t kRescueFlagValidKelf = 0x00000004U;

struct RescueCapsuleInfo {
    std::uint32_t flags{};
    std::uint32_t payload_start{};
    std::uint32_t payload_sectors{};
    std::uint32_t payload_bytes{};
    std::uint32_t kelf_file_bytes{};
    crypto::Sha256Digest apa_sha256{};
    crypto::Sha256Digest payload_sha256{};
    std::string romver;
    std::string family;
    std::string confidence;
};

struct RescueDecodeResult {
    bool ok{};
    std::string error;
    RescueCapsuleInfo info;
};

struct RescueImageResult {
    bool ok{};
    std::string error;
    RescueCapsuleInfo info;
    std::array<std::byte, kRescueApaHeaderBytes> apa_header{};
    std::vector<std::byte> payload;
};

// Exact FHDB Manager HDDRESCUE v1 metadata layout. This is the shared
// PS2HBRC\0 wire format, not a DriveForge-flavored approximation with similar
// intentions and incompatible bytes. The historical PS2 repository is named
// fhdb-bootstrap-manager.
[[nodiscard]] std::array<std::byte, kRescueMetadataBytes>
encode_rescue_metadata(const RescueCapsuleInfo& info);

[[nodiscard]] RescueDecodeResult decode_rescue_metadata(
    std::span<const std::byte, kRescueMetadataBytes> metadata,
    std::size_t complete_file_size);

[[nodiscard]] bool is_standard_apa_master(
    std::span<const std::byte, kRescueApaHeaderBytes> header) noexcept;
[[nodiscard]] bool is_hybrid_gpt_master(
    std::span<const std::byte, kRescueApaHeaderBytes> header) noexcept;

// Disk identity deliberately ignores the APA checksum and osdStart/osdSize.
// Those fields are expected to change during bootstrap recovery. Everything
// else participates so a perfectly valid rescue from another HDD stays a
// perfectly valid file that we still refuse to apply here.
[[nodiscard]] bool same_disk_identity(
    std::span<const std::byte, kRescueApaHeaderBytes> current,
    std::span<const std::byte, kRescueApaHeaderBytes> saved) noexcept;

// Validate a complete HDDRESCUE.BIN/HDDRESCUE2.BIN exactly as FHDB Manager
// does: metadata relationships, APA structure/hash, payload hash and recorded
// KELF length when VALID_KELF is present. Unknown flags and imaginative file
// sizes are rejected instead of interpreted creatively.
[[nodiscard]] RescueImageResult validate_rescue_image(std::span<const std::byte> image);

// Build one canonical Rescue Capsule from exact saved disk bytes. Diagnostic
// strings are supplied by the caller; hashes and validity flags are derived
// here. VALID_KELF is set only when the sector image structurally contains one
// valid KELF and its unpadded size can be recovered.
[[nodiscard]] RescueImageResult build_rescue_image(
    std::span<const std::byte, kRescueApaHeaderBytes> apa_header,
    std::span<const std::byte> payload,
    std::uint32_t payload_start,
    std::uint32_t payload_sectors,
    std::string romver = {},
    std::string family = {},
    std::string confidence = {});

// Serialization is intentionally boring: metadata, exact master, exact
// optional payload. Boring binary formats are considerably easier to exchange
// between a PS2 and a PC than clever ones.
[[nodiscard]] std::vector<std::byte> serialize_rescue_image(const RescueImageResult& image);

} // namespace ps2hdd::fhdb
