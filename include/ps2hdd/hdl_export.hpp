#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/hdl.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace ps2hdd {

// Called after each written chunk with (bytes written so far, ISO size).
using HdlIsoExportProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;

struct HdlIsoExportOptions {
    // Large sequential reads keep old IDE/USB bridges streaming. The export
    // reads chunk N+1 while chunk N is being written to the host.
    std::size_t chunk_bytes = 8U * 1024U * 1024U;
    bool overwrite = false;
};

struct HdlIsoExportResult {
    bool ok{};
    bool cancelled{};
    std::string error;
    std::uint64_t bytes_written{};
    // ISO9660 primary volume descriptor ("CD001" at sector 16) was present.
    bool iso9660_signature{};
    std::vector<std::string> warnings;
};

// Host-safe default filename: "<title> [<startup>].iso".
[[nodiscard]] std::string default_hdl_iso_filename(const hdl::GameInfo& game);

// Stream one validated HDL game image to a host file. The source is only read.
// Data is written to "<destination>.partial" and renamed into place only after
// every byte has been written and flushed, so a cancelled or failed export
// never leaves a truncated file under the final name.
[[nodiscard]] HdlIsoExportResult export_hdl_iso(BlockDevice& device,
                                                const hdl::GameLayout& layout,
                                                const std::filesystem::path& destination,
                                                const HdlIsoExportOptions& options = {},
                                                HdlIsoExportProgress progress = {},
                                                std::stop_token stop = {});

} // namespace ps2hdd
