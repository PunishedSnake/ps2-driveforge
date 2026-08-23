#pragma once

#include "ps2hdd/write_transaction.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace ps2hdd {

// DriveForge's PS2DFRC1 file is a transaction journal for host-side metadata
// mutation. It is deliberately NOT an FHDB Rescue Capsule. FHDB Rescue Capsule
// means only the interoperable PS2HBRC\0 v1 format implemented by fhdb_rescue.
enum class MutationJournalState : std::uint32_t {
    prepared = 1,
    committed = 2,
    restored = 3,
};

enum class MutationDeviceState {
    unknown,
    all_before,
    all_after,
    mixed,
    foreign_or_corrupt,
};

struct MutationJournalResult {
    bool ok{};
    std::string error;
    std::size_t ranges{};
    std::uint64_t captured_bytes{};
};

struct MutationJournalInspection {
    bool ok{};
    std::string error;
    MutationJournalState journal_state{MutationJournalState::prepared};
    MutationDeviceState device_state{MutationDeviceState::unknown};
    std::size_t ranges{};
    std::size_t before_ranges{};
    std::size_t after_ranges{};
    std::size_t unchanged_ranges{};
    std::uint64_t captured_bytes{};
};

struct MutationJournalRestoreResult {
    bool ok{};
    std::string error;
    MutationJournalInspection inspection;
    std::size_t writes_attempted{};
};

// Persist the complete small metadata transaction before the first target
// write. PS2DFRC1 contains exact before/after bytes, offsets and target size.
// It is a DriveForge transaction journal, not a PS2 HDD bootstrap backup.
[[nodiscard]] MutationJournalResult create_mutation_journal(
    const std::filesystem::path& path,
    const WritableBlockDevice& device,
    std::span<const StagedWrite> writes);

// Compare every protected range against the exact before/after images recorded
// in PS2DFRC1. Any third state fails closed as foreign_or_corrupt.
[[nodiscard]] MutationJournalInspection inspect_mutation_journal(
    const std::filesystem::path& path,
    WritableBlockDevice& device);

// Mark a successfully verified PREPARED journal COMMITTED using the same durable
// atomic replacement used for journal creation.
[[nodiscard]] MutationJournalResult mark_mutation_journal_committed(
    const std::filesystem::path& path);

// Restore a PREPARED journal to all before-images in reverse range order,
// flush, byte-verify, then durably mark it RESTORED. COMMITTED journals are
// never automatically rolled back.
[[nodiscard]] MutationJournalRestoreResult restore_prepared_mutation_journal(
    const std::filesystem::path& path,
    WritableBlockDevice& device);

} // namespace ps2hdd
