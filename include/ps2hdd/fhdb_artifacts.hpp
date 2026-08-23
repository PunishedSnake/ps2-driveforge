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

// Exact APAMETA1 bytes shared with FHDB Manager HDDMETA.BIN slots. Do not add
// host-only convenience fields here. An interoperable forensic artifact stops
// being interoperable the moment one side decides the other can probably cope.
[[nodiscard]] std::vector<std::byte> build_hddmeta_image(
    const forensic::ScanResult& scan,
    const forensic::RepairPlan& plan,
    std::string& error);

[[nodiscard]] HddMetaValidation validate_hddmeta_image(std::span<const std::byte> image);

// Canonical two-slot policy shared with FHDB Manager. Existing identical or
// state-equivalent evidence may be reused. Unrelated evidence is never replaced
// merely because slot zero looked convenient.
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

// FORENSIC.TXT is a report rather than irreplaceable binary evidence, so the
// canonical filename is refreshed. DriveForge still writes it durably and reads
// it back because text files are not exempt from storage failures by being easy
// for humans to open.
[[nodiscard]] ArtifactSaveResult save_forensic_report(
    const std::filesystem::path& directory,
    const forensic::ScanResult& scan);

} // namespace ps2hdd::fhdb
