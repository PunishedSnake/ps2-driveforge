#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/fhdb_rescue.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2hdd::fhdb {

enum class BootstrapRestoreKind {
    none,
    rescue_payload,
    legacy_pointer,
};

struct BootstrapRestorePlan {
    bool ok{};
    std::string error;
    BootstrapRestoreKind kind{BootstrapRestoreKind::none};
    std::filesystem::path source_path;

    // Exact live master used for preflight. Apply requires this byte-for-byte
    // snapshot to still be current. Storage software has enough opportunities
    // for races without us inventing another one for entertainment.
    std::array<std::byte, kRescueApaHeaderBytes> source_master{};
    std::array<std::byte, kRescueApaHeaderBytes> saved_master{};
    std::vector<std::byte> payload;

    std::uint32_t payload_start{};
    std::uint32_t payload_sectors{};
    std::string romver;
    std::string family;
    std::string confidence;
};

struct BootstrapRestoreResult {
    bool ok{};
    bool partial{};
    bool rollback_attempted{};
    bool rollback_ok{};
    std::string error;
    std::string warning;
    BootstrapRestoreKind kind{BootstrapRestoreKind::none};
    std::filesystem::path source_path;
    std::filesystem::path safety_backup_path;
    std::uint32_t payload_start{};
    std::uint32_t payload_sectors{};
    std::uint64_t payload_bytes{};
    bool payload_verified{};
    bool pointer_published{};
    bool final_apa_verified{};
};

// Reproduce PS2 HDD Bootstrap Manager restore discovery on the host side.
// HDDRESCUE.BIN/HDDRESCUE2.BIN are preferred. A valid full capsule wins.
// A corrupt, wrong-disk, or payload-without-valid-KELF capsule blocks legacy
// fallback. Only absence or clean header-only capsules permit HDDMBR/FHDBMBR
// pointer-only fallback. Recovery is one place where being "helpful" with a
// suspicious file is just a friendlier spelling of data loss.
[[nodiscard]] BootstrapRestorePlan plan_bootstrap_restore(
    BlockDevice& disk,
    const std::filesystem::path& artifact_directory);

// Apply an already-frozen plan to an image-capable writable device. The current
// master must still match plan.source_master exactly. A mandatory HDDMBR safety
// snapshot is persisted before any device write. Full rescue writes, flushes and
// verifies the payload first, then publishes only osdStart/osdSize plus the APA
// checksum in the current master. The saved master is identity evidence, not a
// convenient excuse to overwrite unrelated current metadata.
[[nodiscard]] BootstrapRestoreResult apply_bootstrap_restore(
    WritableBlockDevice& disk,
    const BootstrapRestorePlan& plan,
    const std::filesystem::path& safety_directory);

struct BootstrapDisableResult {
    bool ok{};
    bool already_disabled{};   // pointer was 0/0; nothing was written
    bool partial{};
    bool rollback_attempted{};
    bool rollback_ok{};
    std::string error;
    std::string warning;
    std::uint32_t previous_start{};
    std::uint32_t previous_sectors{};
    std::filesystem::path hddmbr_path;      // mandatory, saved before any write
    std::filesystem::path rescue_path;      // best effort; empty when capture refused
    std::array<std::byte, kRescueApaHeaderBytes> source_master{};
    bool pointer_cleared{};
    bool final_apa_verified{};
};

// FHDB Manager "disable active bootstrap" parity: persist HDDMBR (mandatory)
// and, independently, try a full Rescue Capsule of whatever the pointer
// currently publishes; then clear only osdStart/osdSize, recompute the APA
// checksum, flush, read back exactly and run a normal APA parse. Partitions,
// games and the payload sectors themselves are untouched, so the pointer can be
// restored later from HDDMBR/HDDRESCUE. A broken or half-written bootstrap is
// the main reason this exists, which is why a failed rescue capture never
// blocks it.
[[nodiscard]] BootstrapDisableResult disable_bootstrap(
    WritableBlockDevice& disk,
    const std::filesystem::path& safety_directory);

[[nodiscard]] BootstrapRestoreResult restore_bootstrap_from_directory_to_image(
    WritableBlockDevice& disk,
    const std::filesystem::path& artifact_directory,
    const std::filesystem::path& safety_directory);

} // namespace ps2hdd::fhdb
