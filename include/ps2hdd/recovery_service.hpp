#pragma once

#include "ps2hdd/structure_health.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ps2hdd::recovery_service {

// One recovery surface for every frontend (WinUI, Win32, future CLI shells).
// A Source is either a disk image or, on Windows, a PhysicalDrive index. Image
// writes go through WritableFileBlockDevice; physical writes go through the
// existing guarded coordinators (admission, short RW lease, cold reopen). The
// service never upgrades evidence: it only picks automatic-safe forensic maps
// and refuses PC-owned disks exactly like the underlying writers.
struct Source {
    std::filesystem::path image;
    std::optional<unsigned> physical;

    [[nodiscard]] bool valid() const noexcept { return physical.has_value() || !image.empty(); }
    [[nodiscard]] std::string name() const;
};

struct Outcome {
    bool ok{};
    bool partial{};
    bool nothing_to_do{};
    bool cold_verified{};
    std::string error;
    std::string warning;
    std::string summary;
    std::vector<std::filesystem::path> artifacts;
};

struct HealthOutcome {
    bool ok{};
    std::string error;
    health::Report report;
    std::string text;  // health::render(report)
};

[[nodiscard]] HealthOutcome assess(const Source& source);

[[nodiscard]] Outcome capture_rescue(const Source& source, const std::filesystem::path& artifact_directory);
[[nodiscard]] Outcome restore_bootstrap(const Source& source,
                                        const std::filesystem::path& artifact_directory,
                                        const std::filesystem::path& safety_directory);
[[nodiscard]] Outcome disable_bootstrap(const Source& source, const std::filesystem::path& safety_directory);
[[nodiscard]] Outcome repair_master(const Source& source, const std::filesystem::path& artifact_directory);
// Read-only: FORENSIC.TXT always, HDDMETA.BIN for the best automatic-safe map.
[[nodiscard]] Outcome forensic_report(const Source& source, const std::filesystem::path& artifact_directory);
[[nodiscard]] Outcome repair_forensic_automatic(const Source& source,
                                                const std::filesystem::path& artifact_directory);

} // namespace ps2hdd::recovery_service
