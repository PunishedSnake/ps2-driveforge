#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"
#include "ps2hdd/hdl.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace ps2hdd::hdl {

struct ImageInstallOptions {
    std::string title;
    bool hidden{};
    std::uint8_t compat_flags{};
    std::uint16_t dma{};
    std::uint32_t layer_break{};
    MediaType media{MediaType::dvd};
    apa::Ps2Time created{};
    std::size_t copy_buffer_bytes{4U * 1024U * 1024U};

    // Optional host-side PS2DFRC1 transaction journal for the small
    // DEADFEED/APA publication transaction. This is NOT an FHDB PS2HBRC Rescue
    // Capsule. Empty keeps pure in-memory/test devices free of filesystem side
    // effects. Real image-file frontends should provide a sidecar path.
    std::filesystem::path mutation_journal_path;
};

struct ImageInstallProgress {
    std::uint64_t copied_bytes{};
    std::uint64_t total_bytes{};
    std::string_view phase;
};

using ImageInstallProgressCallback = std::function<void(const ImageInstallProgress&)>;

struct ImageInstallResult {
    bool ok{};
    std::string error;
    std::string warning;
    std::string partition_id;
    std::string startup;
    std::uint32_t main_start_lba{};
    std::size_t sub_count{};
    std::uint64_t payload_bytes{};
    std::uint64_t allocated_bytes{};
    std::uint64_t orphan_payload_bytes_on_failure{};
    bool mutation_journal_created{};
    bool mutation_recovery_pending{};
};

// Frieren F4 experimental installer for writable *images*. The API accepts a
// WritableBlockDevice deliberately, so callers cannot hand it the read-only
// PhysicalDrive type used by normal DriveForge sessions.
//
// Ordering is safety-critical:
//   1. clean APA + ISO preflight and zero-write allocation plan;
//   2. stream and byte-verify ISO payload into currently free extents;
//   3. flush payload;
//   4. stage DEADFEED metadata + new APA headers + old-neighbour link updates;
//   5. when configured, durably persist exact before/after ranges in PS2DFRC1;
//   6. publish them in one WriteTransaction and verify through normal parsers;
//   7. durably mark the mutation journal COMMITTED after parser verification.
//
// A failure before publication can leave bytes in free space, but no APA
// partition is published. A returned publication failure rolls metadata/header
// before-images back. A process/power interruption is recoverable when the
// caller supplied mutation_journal_path; a PREPARED journal is never silently
// overwritten by a later install.
[[nodiscard]] ImageInstallResult install_to_image(
    WritableBlockDevice& disk,
    BlockDevice& game_iso,
    const ImageInstallOptions& options,
    ImageInstallProgressCallback progress = {});

} // namespace ps2hdd::hdl
