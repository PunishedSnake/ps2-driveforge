#pragma once

#include "ps2hdd/write_transaction.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace ps2hdd {

enum class RecoveryCapsuleState : std::uint32_t {
    prepared = 1,
    committed = 2,
    restored = 3,
};

enum class RecoveryDeviceState {
    unknown,
    all_before,
    all_after,
    mixed,
    foreign_or_corrupt,
};

struct RecoveryCapsuleResult {
    bool ok{};
    std::string error;
    std::size_t ranges{};
    std::uint64_t captured_bytes{};
};

struct RecoveryInspection {
    bool ok{};
    std::string error;
    RecoveryCapsuleState capsule_state{RecoveryCapsuleState::prepared};
    RecoveryDeviceState device_state{RecoveryDeviceState::unknown};
    std::size_t ranges{};
    std::size_t before_ranges{};
    std::size_t after_ranges{};
    std::size_t unchanged_ranges{};
    std::uint64_t captured_bytes{};
};

struct RecoveryRestoreResult {
    bool ok{};
    std::string error;
    RecoveryInspection inspection;
    std::size_t writes_attempted{};
};

// Persist the complete metadata transaction before the first target write. The
// capsule contains exact before/after bytes, offsets and target size. Creation
// uses an atomic temporary-file replacement plus an OS flush boundary so an
// interrupted capsule write cannot masquerade as a valid recovery record.
[[nodiscard]] RecoveryCapsuleResult create_recovery_capsule(
    const std::filesystem::path& path,
    const WritableBlockDevice& device,
    std::span<const StagedWrite> writes);

// Compare every protected range on the current device against the capsule's
// exact before and after images. A range matching neither makes the target
// foreign_or_corrupt and automatic restoration is refused.
[[nodiscard]] RecoveryInspection inspect_recovery_capsule(
    const std::filesystem::path& path,
    WritableBlockDevice& device);

// Rewrite only the capsule state using the same durable atomic replacement.
// Call this after the protected mutation and its format-level verifier succeed.
[[nodiscard]] RecoveryCapsuleResult mark_recovery_capsule_committed(
    const std::filesystem::path& path);

// Restore a PREPARED capsule to all before-images in reverse range order,
// flush, byte-verify, then durably mark the capsule RESTORED. COMMITTED capsules
// and devices with any range matching neither before nor after are refused.
[[nodiscard]] RecoveryRestoreResult restore_prepared_recovery_capsule(
    const std::filesystem::path& path,
    WritableBlockDevice& device);

} // namespace ps2hdd
