#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"
#include "ps2hdd/hdl.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
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
    std::string partition_id;
    std::string startup;
    std::uint32_t main_start_lba{};
    std::size_t sub_count{};
    std::uint64_t payload_bytes{};
    std::uint64_t allocated_bytes{};
    std::uint64_t orphan_payload_bytes_on_failure{};
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
//   5. publish them in one WriteTransaction and verify through normal parsers.
//
// A failure before step 5 can leave bytes in free space, but no APA partition is
// published. A failure during step 5 rolls metadata/header before-images back.
[[nodiscard]] ImageInstallResult install_to_image(
    WritableBlockDevice& disk,
    BlockDevice& game_iso,
    const ImageInstallOptions& options,
    ImageInstallProgressCallback progress = {});

} // namespace ps2hdd::hdl
