#pragma once

#ifdef _WIN32

#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/apa_recovery_apply.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace ps2hdd {

struct PhysicalApaRecoveryOptions {
    std::filesystem::path artifact_directory;
    bool lock_mounted_volumes{true};
    bool dismount_locked_volumes{true};
};

struct PhysicalMasterRepairResult {
    bool ok{};
    bool partial{};
    std::string error;
    std::size_t locked_volume_count{};
    bool cold_master_verified{};
    bool cold_apa_clean{};
    recovery::RepairResult repair;
};

struct PhysicalForensicRepairResult {
    bool ok{};
    bool partial{};
    std::string error;
    std::size_t locked_volume_count{};
    bool cold_touched_set_verified{};
    bool cold_apa_clean{};
    recovery::RepairResult repair;
};

// Narrow sector-zero recovery for a damaged APA master. This uses exceptional
// physical authorization because a disk needing master repair may correctly be
// rejected by the ordinary APA reader. The existing repair::ApaRepairPlan still
// decides whether the bytes are safe to change; exceptional authorization only
// proves that the same non-GPT-owned physical device is receiving them.
[[nodiscard]] PhysicalMasterRepairResult repair_master_header_on_physical(
    unsigned physical_drive_index,
    const PhysicalApaRecoveryOptions& options);

// Apply an already frozen forensic scan/plan to a physical HDD. The caller owns
// map selection and any explicit manual authorization. This coordinator does not
// upgrade confidence, choose a nicer topology or rebuild the plan after the write
// lease opens. Recovery evidence is not a democracy and wanting a map strongly
// enough does not make it automatic-safe.
[[nodiscard]] PhysicalForensicRepairResult repair_forensic_topology_on_physical(
    unsigned physical_drive_index,
    const forensic::ScanResult& scan,
    const forensic::RepairPlan& plan,
    const PhysicalApaRecoveryOptions& options,
    bool allow_manual = false);

} // namespace ps2hdd

#endif
