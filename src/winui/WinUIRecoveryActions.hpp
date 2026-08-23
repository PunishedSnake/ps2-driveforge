#pragma once

#include "NativeSessionController.hpp"
#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/physical_apa_recovery.hpp"
#include "ps2hdd/physical_drive.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

namespace ps2df::winui {

struct ForensicActionSnapshot {
    bool ok{};
    bool partial{};
    bool automatic_safe{};
    std::string error;
    std::string summary;
    std::filesystem::path report_path;
    std::filesystem::path metadata_path;
    std::size_t node_count{};
    std::size_t map_count{};
    std::size_t selected_map{};
    unsigned confidence{};
    bool cold_verified{};
};

[[nodiscard]] inline ForensicActionSnapshot analyze_physical_apa(
    unsigned physical_index,
    const std::filesystem::path& artifact_directory)
{
    ForensicActionSnapshot result;
    if (artifact_directory.empty()) {
        result.error = "Choose a host-side safety/artifact directory before forensic analysis.";
        return result;
    }

    ps2hdd::PhysicalDrive disk(physical_index);
    if (!disk.is_open()) {
        result.error = "Could not reopen PhysicalDrive" + std::to_string(physical_index) + " read-only.";
        return result;
    }

    const auto scan = ps2hdd::forensic::scan_apa(disk);
    if (!scan.ok) {
        result.error = scan.error;
        return result;
    }
    result.node_count = scan.nodes.size();
    result.map_count = scan.maps.size();

    const auto report = ps2hdd::fhdb::save_forensic_report(artifact_directory, scan);
    if (!report.ok) {
        result.error = report.error;
        return result;
    }
    result.report_path = report.path;

    std::optional<std::size_t> best;
    unsigned best_confidence = 0;
    for (std::size_t i = 0; i < scan.maps.size(); ++i) {
        const auto plan = ps2hdd::forensic::build_repair_plan(scan, i);
        if (!scan.maps[i].repairable || !plan.automatic_safe) continue;
        if (!best || plan.confidence > best_confidence) {
            best = i;
            best_confidence = plan.confidence;
        }
    }
    if (best) {
        const auto plan = ps2hdd::forensic::build_repair_plan(scan, *best);
        result.selected_map = *best;
        result.confidence = plan.confidence;
        result.automatic_safe = plan.automatic_safe;
        const auto meta = ps2hdd::fhdb::save_hddmeta(artifact_directory, scan, plan);
        if (!meta.ok) {
            result.error = meta.error;
            return result;
        }
        result.metadata_path = meta.path;
    }

    result.summary = best
        ? "Forensic APA analysis exported; an automatic-safe repair map is available"
        : "Forensic APA analysis exported; no automatic-safe repair map was found";
    result.ok = true;
    return result;
}

[[nodiscard]] inline ForensicActionSnapshot repair_physical_master(
    unsigned physical_index,
    const std::filesystem::path& artifact_directory)
{
    ForensicActionSnapshot result;
    if (artifact_directory.empty()) {
        result.error = "Master repair requires a host-side safety artifact directory.";
        return result;
    }
    ps2hdd::PhysicalApaRecoveryOptions options;
    options.artifact_directory = artifact_directory;
    const auto repaired = ps2hdd::repair_master_header_on_physical(physical_index, options);
    result.ok = repaired.ok;
    result.partial = repaired.partial;
    result.error = repaired.error;
    result.cold_verified = repaired.cold_master_verified && repaired.cold_apa_clean;
    result.summary = repaired.ok
        ? "Conservative APA master repair committed and cold-verified"
        : "APA master repair did not complete";
    return result;
}

[[nodiscard]] inline ForensicActionSnapshot repair_physical_apa_automatic(
    unsigned physical_index,
    const std::filesystem::path& artifact_directory)
{
    ForensicActionSnapshot result;
    if (artifact_directory.empty()) {
        result.error = "Forensic topology repair requires a host-side safety artifact directory.";
        return result;
    }

    // Freeze scan and map before exceptional write admission. We deliberately
    // refuse manual-only maps here. A GUI button is not additional evidence.
    ps2hdd::PhysicalDrive disk(physical_index);
    if (!disk.is_open()) {
        result.error = "Could not reopen the physical disk read-only for forensic planning.";
        return result;
    }
    const auto scan = ps2hdd::forensic::scan_apa(disk);
    if (!scan.ok) {
        result.error = scan.error;
        return result;
    }
    result.node_count = scan.nodes.size();
    result.map_count = scan.maps.size();

    std::optional<std::size_t> best;
    ps2hdd::forensic::RepairPlan best_plan;
    for (std::size_t i = 0; i < scan.maps.size(); ++i) {
        if (!scan.maps[i].repairable) continue;
        const auto plan = ps2hdd::forensic::build_repair_plan(scan, i);
        if (!plan.automatic_safe) continue;
        if (!best || plan.confidence > best_plan.confidence) {
            best = i;
            best_plan = plan;
        }
    }
    if (!best) {
        result.error = "No automatic-safe forensic topology repair is available. Manual-only evidence is intentionally not writable from WinUI.";
        return result;
    }

    const auto report = ps2hdd::fhdb::save_forensic_report(artifact_directory, scan);
    if (!report.ok) {
        result.error = report.error;
        return result;
    }
    const auto meta = ps2hdd::fhdb::save_hddmeta(artifact_directory, scan, best_plan);
    if (!meta.ok) {
        result.error = meta.error;
        return result;
    }
    result.report_path = report.path;
    result.metadata_path = meta.path;
    result.selected_map = *best;
    result.confidence = best_plan.confidence;
    result.automatic_safe = true;

    ps2hdd::PhysicalApaRecoveryOptions options;
    options.artifact_directory = artifact_directory;
    const auto repaired = ps2hdd::repair_forensic_topology_on_physical(
        physical_index, scan, best_plan, options, false);
    result.ok = repaired.ok;
    result.partial = repaired.partial;
    result.error = repaired.error;
    result.cold_verified = repaired.cold_touched_set_verified && repaired.cold_apa_clean;
    result.summary = repaired.ok
        ? "Automatic-safe forensic topology repair committed and cold-verified"
        : "Forensic topology repair did not complete";
    return result;
}

} // namespace ps2df::winui
