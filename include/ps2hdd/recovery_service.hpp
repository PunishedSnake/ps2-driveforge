#pragma once

#include "ps2hdd/apa_migrate.hpp"
#include "ps2hdd/boot_chain.hpp"
#include "ps2hdd/structure_health.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
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
    std::string details;  // multi-line report (e.g. forensic map previews)
    std::vector<std::filesystem::path> artifacts;
};

struct HealthOutcome {
    bool ok{};
    std::string error;
    health::Report report;
    std::string text;  // health::render(report)
};

[[nodiscard]] HealthOutcome assess(const Source& source);

struct BootChainOutcome {
    bool ok{};
    std::string error;
    bootchain::Report report;
    std::string text;
};
[[nodiscard]] BootChainOutcome boot_chain(const Source& source, const std::filesystem::path& memory_card_root = {});

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

// Drive migration: PS2 HDD -> bigger drive, -> sparse backup image, or a
// backup image -> drive. Image targets are created new (never overwritten).
struct MigrationTarget {
    std::filesystem::path image;
    std::uint64_t image_bytes{};  // size for a new image target
    std::optional<unsigned> physical;

    [[nodiscard]] std::string name() const;
};

struct MigrationPlanOutcome {
    bool ok{};
    std::string error;
    migrate::Plan plan;
    std::string text;
};

[[nodiscard]] MigrationPlanOutcome plan_migration(const Source& source, const MigrationTarget& target);
[[nodiscard]] Outcome migrate(const Source& source, const MigrationTarget& target, const migrate::Plan& plan,
                              migrate::Progress progress = {}, std::stop_token stop = {});
[[nodiscard]] std::string human_bytes(std::uint64_t bytes);

} // namespace ps2hdd::recovery_service
