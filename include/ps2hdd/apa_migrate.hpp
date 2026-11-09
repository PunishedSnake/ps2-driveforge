#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace ps2hdd::migrate {

// Clone a PS2 HDD onto another disk (bigger drive, smaller-but-big-enough
// drive, or a sparse backup image) by copying only what the APA chain owns.
//
// APA addresses partitions by absolute LBA and libapa computes free space from
// the device's real totalLBA (iop/hdd/libapa/src/free.c), while the __mbr
// header stores no disk size (mbr.nsector is always 0). So a byte-exact copy of
// every chain partition at the same LBA is a complete migration: the space past
// the last partition simply becomes allocatable on the new disk. "__empty"
// partitions are copied header-only because their payload is free space.
//
// Crash safety: every range except the APA master (sectors 0-1) is copied,
// flushed and verified first; the master is written last. An interrupted
// migration therefore leaves a target that no PS2 tool accepts as an APA disk,
// instead of a half-copied disk that looks valid.

struct Range {
    std::uint64_t offset{};  // bytes, identical on source and target
    std::uint64_t bytes{};
    std::string what;        // partition id(s)
};

struct Plan {
    bool ok{};
    std::string error;
    std::vector<std::string> warnings;
    std::uint64_t source_bytes{};
    std::uint64_t target_bytes{};
    std::uint64_t required_bytes{};  // end of the last allocated partition
    std::uint64_t copy_bytes{};
    std::size_t partitions{};
    std::vector<Range> ranges;       // sorted, merged, non-overlapping
    std::array<std::byte, 1024> master{};

    [[nodiscard]] std::int64_t gained_bytes() const noexcept
    {
        return static_cast<std::int64_t>(target_bytes) - static_cast<std::int64_t>(source_bytes);
    }
};

struct Options {
    std::size_t chunk_bytes{8U * 1024U * 1024U};
    // The target is known to read back zeros everywhere (a freshly created
    // sparse image): all-zero chunks are skipped, keeping backups sparse.
    bool target_is_zeroed{false};
    bool verify{true};  // re-read the target and compare SHA-256 per range
};

struct Result {
    bool ok{};
    bool cancelled{};
    bool master_written{};  // false => target is still not an APA disk
    std::string error;
    std::uint64_t copied_bytes{};
    std::uint64_t skipped_zero_bytes{};
    bool verified{};
};

using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;

// target_bytes is the capacity of the destination. Refuses damaged APA chains
// (repair first), PC-owned disks and APA-Jail (its exFAT partition sits at a
// fixed position after the APA region and is not part of the chain).
[[nodiscard]] Plan plan(BlockDevice& source, std::uint64_t target_bytes);

[[nodiscard]] Result apply(BlockDevice& source, WritableBlockDevice& target, const Plan& plan,
                           const Options& options = {}, Progress progress = {}, std::stop_token stop = {});

// Creates a new sparse image of `bytes` (refuses to replace an existing file).
[[nodiscard]] bool create_sparse_image(const std::filesystem::path& path, std::uint64_t bytes, std::string& error);

} // namespace ps2hdd::migrate
