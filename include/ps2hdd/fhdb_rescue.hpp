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

// Exact PS2 HDD Bootstrap Manager HDDRESCUE v1 metadata layout. The emitted
// bytes are intentionally interoperable with capsule_format.c from that tool.
[[nodiscard]] std::array<std::byte, kRescueMetadataBytes>
encode_rescue_metadata(const RescueCapsuleInfo& info);

[[nodiscard]] RescueDecodeResult decode_rescue_metadata(
    std::span<const std::byte, kRescueMetadataBytes> metadata,
    std::size_t complete_file_size);

[[nodiscard]] bool is_standard_apa_master(
    std::span<const std::byte, kRescueApaHeaderBytes> header) noexcept;
[[nodiscard]] bool is_hybrid_gpt_master(
    std::span<const std::byte, kRescueApaHeaderBytes> header) noexcept;
[[nodiscard]] bool same_disk_identity(
    std::span<const std::byte, kRescueApaHeaderBytes> current,
    std::span<const std::byte, kRescueApaHeaderBytes> saved) noexcept;

// Validate a complete HDDRESCUE.BIN/HDDRESCUE2.BIN image exactly as the PS2
// manager does: metadata relationships, APA digest/structure, payload digest,
// and recorded KELF length when VALID_KELF is present.
[[nodiscard]] RescueImageResult validate_rescue_image(std::span<const std::byte> image);

// Build a complete canonical rescue image from exact saved disk bytes. The
// caller supplies diagnostic strings only; hashes and flags for APA/payload are
// derived here. VALID_KELF is set only when the sector image structurally
// contains one valid KELF and its unpadded byte count can be recovered.
[[nodiscard]] RescueImageResult build_rescue_image(
    std::span<const std::byte, kRescueApaHeaderBytes> apa_header,
    std::span<const std::byte> payload,
    std::uint32_t payload_start,
    std::uint32_t payload_sectors,
    std::string romver = {},
    std::string family = {},
    std::string confidence = {});

[[nodiscard]] std::vector<std::byte> serialize_rescue_image(const RescueImageResult& image);

} // namespace ps2hdd::fhdb
