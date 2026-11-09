#pragma once

#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/apa_repair.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
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

// Exceptional sector-zero repair. Save and read back canonical FHDB HDDRAW
// evidence first, then recheck source stability and the conservative repair
// plan before touching the master. Image-only until the physical-write gate is
// intentionally earned. Sector zero already has enough mythology around it.
[[nodiscard]] RepairResult repair_master_header(
    WritableBlockDevice& device,
    const std::filesystem::path& artifact_directory);

// Apply one frozen forensic topology plan. Automatic-safe plans are accepted by
// default; speculative/manual plans require explicit allow_manual. HDDMETA and
// FORENSIC.TXT must exist before writes. Every source header is reread before its
// mutation, each write is flushed/read back, non-master headers go first and LBA
// 0 goes last. Writing the master first would make a bad interruption much more
// interesting than anyone requested.
[[nodiscard]] RepairResult repair_forensic_topology(
    WritableBlockDevice& device,
    const forensic::ScanResult& scan,
    const forensic::RepairPlan& plan,
    const std::filesystem::path& artifact_directory,
    bool allow_manual = false);

} // namespace ps2hdd::recovery
