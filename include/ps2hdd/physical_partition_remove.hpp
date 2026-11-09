#pragma once

#ifdef _WIN32

#include "ps2hdd/apa_remove.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ps2hdd {

struct PhysicalPartitionRemoveOptions {
    std::filesystem::path artifact_directory;
    bool lock_mounted_volumes{true};
    bool dismount_locked_volumes{true};
};

struct PhysicalPartitionRemoveResult {
    bool ok{};
    bool recovery_pending{};
    std::string error;
    std::filesystem::path hddmbr_path;
    std::filesystem::path mutation_journal_path;
    std::size_t locked_volume_count{};
    apa::RemoveResult removal;
};

// Remove one physical APA main plus its authoritative subpartitions. The disk
// payload is not zero-filled. A detached game is free space, not a moral lesson
// requiring several gigabytes of zeros.
//
// Safety order mirrors physical game deployment: read-only admission and plan,
// FHDB-compatible master backup, guarded RW lease, PS2DFRC1 before the first
// chain-link write, commit/readback, release RW, then cold read-only verification.
[[nodiscard]] PhysicalPartitionRemoveResult remove_partition_from_physical(
    unsigned physical_drive_index,
    std::uint32_t main_lba,
    const PhysicalPartitionRemoveOptions& options);

} // namespace ps2hdd

#endif
