#pragma once

#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/fhdb_rescue.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::fhdb {

struct ArtifactSaveResult {
    bool ok{};
    bool reused{};
    std::filesystem::path path;
    std::string error;
};

struct HddMetaValidation {
    bool ok{};
    std::string error;
    std::uint32_t total_sectors{};
    std::uint32_t map_index{};
    std::uint32_t confidence{};
    std::uint32_t patch_count{};
    std::uint32_t corroborated_count{};
    std::uint32_t speculative_count{};
    std::uint32_t one_or_two_bit_count{};
};

// Exact APAMETA1 bytes used by fhdb-bootstrap-manager HDDMETA.BIN slots.
[[nodiscard]] std::vector<std::byte> build_hddmeta_image(
    const forensic::ScanResult& scan,
    const forensic::RepairPlan& plan,
    std::string& error);

[[nodiscard]] HddMetaValidation validate_hddmeta_image(std::span<const std::byte> image);

// Canonical non-overwriting two-slot artifacts. Existing identical/state-
// equivalent content is reused; different existing content is never replaced.
[[nodiscard]] ArtifactSaveResult save_hddraw(
    const std::filesystem::path& directory,
    std::span<const std::byte, kRescueApaHeaderBytes> raw_header);
[[nodiscard]] ArtifactSaveResult save_hddmbr(
    const std::filesystem::path& directory,
    std::span<const std::byte, kRescueApaHeaderBytes> raw_header);
[[nodiscard]] ArtifactSaveResult save_hddrescue(
    const std::filesystem::path& directory,
    const RescueImageResult& rescue);
[[nodiscard]] ArtifactSaveResult save_hddmeta(
    const std::filesystem::path& directory,
    const forensic::ScanResult& scan,
    const forensic::RepairPlan& plan);

// Canonical single report filename. Unlike snapshot slots, the PS2 manager
// refreshes FORENSIC.TXT on export; DriveForge mirrors that behavior and adds
// a durable read-back check.
[[nodiscard]] ArtifactSaveResult save_forensic_report(
    const std::filesystem::path& directory,
    const forensic::ScanResult& scan);

} // namespace ps2hdd::fhdb
