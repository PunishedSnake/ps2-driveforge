#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"
#include "ps2hdd/disk_layout_guard.hpp"
#include "ps2hdd/sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ps2hdd::physical_write {

inline constexpr std::size_t kIdentityWindowCount = 5;
inline constexpr std::size_t kIdentityWindowBytes = 256U * 1024U;

struct IdentitySnapshot {
    std::uint64_t size_bytes{};
    std::array<std::uint64_t, kIdentityWindowCount> offsets{};
    crypto::Sha256Digest digest{};
};

struct IdentityResult {
    bool ok{};
    std::string error;
    IdentitySnapshot snapshot;
};

// PhysicalDriveN is Windows enumeration state, not disk identity. Capture a
// deterministic digest from several widely separated windows so a later RW
// reopen can prove it is still looking at the source that passed preflight.
// One integer from SetupAPI is a useful address, not a blood oath.
[[nodiscard]] IdentityResult capture_identity(BlockDevice& device);

[[nodiscard]] bool verify_identity(BlockDevice& device,
                                   const IdentitySnapshot& expected,
                                   std::string& error);

struct Authorization {
    IdentitySnapshot identity;
    std::uint32_t apa_version{};
    std::size_t apa_header_count{};
    LegacyPartitionMapKind pc_layout{LegacyPartitionMapKind::none};
};

struct AuthorizationResult {
    bool ok{};
    std::string error;
    Authorization authorization;
};

// Portable read-only admission for future physical mutation. It intentionally
// combines the PC partition-map guard, a clean canonical APA scan and the media
// fingerprint. Passing one of those checks is not a coupon for skipping the
// others simply because sector zero looks familiar.
[[nodiscard]] AuthorizationResult authorize(BlockDevice& device);

// Recheck every admission property immediately before opening the write gate.
// This is useful both for the Windows backend and deterministic image tests.
[[nodiscard]] bool verify_authorization(BlockDevice& device,
                                        const Authorization& expected,
                                        std::string& error);

} // namespace ps2hdd::physical_write
