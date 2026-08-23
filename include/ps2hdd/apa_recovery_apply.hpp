#pragma once

#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/apa_repair.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace ps2hdd::recovery {

struct RepairResult {
    bool ok{};
    bool partial{};
    std::string error;
    std::filesystem::path snapshot_path;
    std::filesystem::path forensic_report_path;
    std::size_t writes_completed{};
};

// Exceptional sector-zero repair. The current 1024-byte master is saved to the
// canonical FHDB HDDRAW slot and read back before source stability and the
// conservative planner are rechecked. Image-only until the physical-write gate
// is intentionally opened in a later milestone.
[[nodiscard]] RepairResult repair_master_header(
    WritableBlockDevice& device,
    const std::filesystem::path& artifact_directory);

// Apply one already-built forensic topology plan. Automatic-safe plans are
// accepted by default; speculative/manual plans require explicit allow_manual.
// HDDMETA and FORENSIC.TXT are persisted before writes. Non-master headers are
// written first and LBA 0 last, with exact source stability, flush and readback
// on every header.
[[nodiscard]] RepairResult repair_forensic_topology(
    WritableBlockDevice& device,
    const forensic::ScanResult& scan,
    const forensic::RepairPlan& plan,
    const std::filesystem::path& artifact_directory,
    bool allow_manual = false);

} // namespace ps2hdd::recovery
