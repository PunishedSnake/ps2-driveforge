#pragma once

#include "ps2hdd/pfs.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd::pfs {

struct ExportStats {
    std::uint64_t files{};
    std::uint64_t directories{};
    std::uint64_t bytes{};
    std::uint64_t skipped{};
};

using ExportProgress = std::function<void(const ExportStats&, std::string_view)>;

struct ExportResult {
    bool ok{};
    ExportStats stats{};
    std::string error;
    std::vector<std::string> warnings;
};

// Convert one PFS path component into a host-safe filename. This primarily
// targets Windows restrictions because DriveForge is Windows-first, while
// remaining safe to use on other host platforms.
[[nodiscard]] std::string sanitize_host_filename(std::string_view name);

// Export a PFS file or directory to an exact host destination path. Directory
// export is recursive. The PFS source remains read-only; only host-side files
// and directories are created.
[[nodiscard]] ExportResult export_to_host(Reader& reader,
                                          std::string_view source_path,
                                          const std::filesystem::path& destination,
                                          ExportProgress progress = {});

} // namespace ps2hdd::pfs
