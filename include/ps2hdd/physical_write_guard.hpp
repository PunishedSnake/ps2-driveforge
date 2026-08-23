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

enum class AuthorizationKind {
    normal_mutation,
    exceptional_recovery,
};

struct Authorization {
    AuthorizationKind kind{AuthorizationKind::normal_mutation};
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

// Normal write admission requires PC-layout ownership to be PS2-compatible, a
// clean canonical APA scan and a stable media fingerprint.
[[nodiscard]] AuthorizationResult authorize(BlockDevice& device);

// Exceptional recovery deliberately does NOT require the normal APA reader to
// accept the disk. A sector-zero or topology repair whose first prerequisite is
// "please already have a healthy APA" would be impressively circular. This gate
// proves device size/alignment, absence of conflicting GPT ownership and media
// identity. The repair/forensic planner remains responsible for authorizing the
// actual recovery bytes.
[[nodiscard]] AuthorizationResult authorize_exceptional_recovery(BlockDevice& device);

// Recheck the properties appropriate to the authorization kind immediately
// before opening the write gate. Normal mutation rechecks clean APA topology;
// exceptional recovery rechecks only PC ownership plus exact media identity.
[[nodiscard]] bool verify_authorization(BlockDevice& device,
                                        const Authorization& expected,
                                        std::string& error);

} // namespace ps2hdd::physical_write
