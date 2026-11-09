#ifdef _WIN32

#include "ps2hdd/physical_apa_recovery.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_repair.hpp"
#include "ps2hdd/fhdb_rescue.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/writable_physical_drive.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ps2hdd {
namespace {

constexpr std::uint64_t kSectorBytes = 512ULL;

[[nodiscard]] bool read_header(BlockDevice& disk,
                               std::uint32_t lba,
                               std::array<std::byte, forensic::kApaHeaderBytes>& out) noexcept
{
    const auto offset = static_cast<std::uint64_t>(lba) * kSectorBytes;
    return offset <= disk.size_bytes() && out.size() <= disk.size_bytes() - offset &&
           disk.read(offset, out);
}

struct ExpectedHeader {
    std::uint32_t lba{};
    std::array<std::byte, forensic::kApaHeaderBytes> bytes{};
};

} // namespace

PhysicalMasterRepairResult repair_master_header_on_physical(
    unsigned physical_drive_index,
    const PhysicalApaRecoveryOptions& options)
{
    PhysicalMasterRepairResult result;
    if (options.artifact_directory.empty()) {
        result.error = "Physical APA master repair requires an artifact directory";
        return result;
    }

    PhysicalDrive read_only(physical_drive_index);
    if (!read_only.is_open()) {
        result.error = "Could not open selected PhysicalDrive read-only before APA master repair";
        return result;
    }

    const auto authorization = physical_write::authorize_exceptional_recovery(read_only);
    if (!authorization.ok) {
        result.error = "Exceptional physical recovery admission failed: " + authorization.error;
        return result;
    }

    // Freeze the exact repair bytes before the RW lease opens. The core executor
    // repeats this analysis after saving HDDRAW and immediately before mutation;
    // this first copy exists for independent cold verification after every write
    // handle has been destroyed.
    std::array<std::byte, repair::kApaHeaderBytes> source{};
    if (!read_only.read(0, source)) {
        result.error = "Could not read exact APA master sectors 0-1 during physical recovery preflight";
        return result;
    }
    const auto plan = repair::analyze_apa_master(source);
    if (!plan.header_patch_safe || plan.blockers != 0 || plan.safe_header_fixes == 0) {
        result.error = "APA master damage is not an unambiguous FHDB-safe canonical repair";
        return result;
    }
    std::array<std::byte, repair::kApaHeaderBytes> expected{};
    std::string build_error;
    if (!repair::build_repaired_apa_master(source, plan, expected, build_error)) {
        result.error = "Could not freeze expected repaired APA master: " + build_error;
        return result;
    }

    {
        WritablePhysicalDriveOptions writable_options;
        writable_options.authorization = authorization.authorization;
        writable_options.lock_mounted_volumes = options.lock_mounted_volumes;
        writable_options.dismount_locked_volumes = options.dismount_locked_volumes;

        WritablePhysicalDrive writable(physical_drive_index, writable_options);
        if (!writable.is_open()) {
            result.error = "Could not acquire guarded exceptional physical recovery lease: " +
                           writable.admission_error();
            return result;
        }
        result.locked_volume_count = writable.locked_volume_count();
        result.repair = recovery::repair_master_header(writable, options.artifact_directory);
        if (!result.repair.ok) {
            result.partial = result.repair.partial;
            result.error = "Physical APA master repair failed: " + result.repair.error;
            return result;
        }
    }

    PhysicalDrive cold(physical_drive_index);
    if (!cold.is_open()) {
        result.partial = true;
        result.error = "APA master repair completed, but cold read-only PhysicalDrive reopen failed";
        return result;
    }
    std::array<std::byte, repair::kApaHeaderBytes> actual{};
    if (!cold.read(0, actual) || actual != expected || !fhdb::is_standard_apa_master(actual)) {
        result.partial = true;
        result.error = "APA master repair completed, but cold exact/canonical master verification failed";
        return result;
    }
    result.cold_master_verified = true;

    // A narrowly repaired master may coexist with unrelated topology damage.
    // Report whether the complete normal APA reader is clean, but do not rewrite
    // the meaning of a successful master-only operation by pretending it also
    // repaired every other header on disk.
    apa::Reader reader(cold);
    result.cold_apa_clean = reader.scan().ok();
    result.ok = true;
    return result;
}

PhysicalForensicRepairResult repair_forensic_topology_on_physical(
    unsigned physical_drive_index,
    const forensic::ScanResult& scan,
    const forensic::RepairPlan& plan,
    const PhysicalApaRecoveryOptions& options,
    bool allow_manual)
{
    PhysicalForensicRepairResult result;
    if (options.artifact_directory.empty()) {
        result.error = "Physical forensic APA repair requires an artifact directory";
        return result;
    }
    if (!scan.ok || scan.truncated || plan.map_index >= scan.maps.size() || plan.patches.empty()) {
        result.error = "Physical forensic repair requires a complete frozen scan and non-empty plan";
        return result;
    }
    if (!plan.automatic_safe && !(allow_manual && plan.manual_allowed)) {
        result.error = "Physical forensic repair plan is not automatic-safe and lacks explicit manual authorization";
        return result;
    }

    PhysicalDrive read_only(physical_drive_index);
    if (!read_only.is_open()) {
        result.error = "Could not open selected PhysicalDrive read-only before forensic APA repair";
        return result;
    }
    if (read_only.size_bytes() / kSectorBytes != scan.total_sectors) {
        result.error = "Frozen forensic scan sector count does not match the selected physical device";
        return result;
    }

    const auto authorization = physical_write::authorize_exceptional_recovery(read_only);
    if (!authorization.ok) {
        result.error = "Exceptional physical recovery admission failed: " + authorization.error;
        return result;
    }

    std::vector<ExpectedHeader> expected;
    expected.reserve(plan.patches.size());
    for (const auto& patch : plan.patches) {
        if (patch.node_index >= scan.nodes.size() || scan.nodes[patch.node_index].lba != patch.lba) {
            result.error = "Frozen forensic plan no longer resolves to its scan nodes";
            return result;
        }
        const auto& node = scan.nodes[patch.node_index];
        std::array<std::byte, forensic::kApaHeaderBytes> current{};
        if (!read_header(read_only, patch.lba, current) || current != node.header) {
            result.error = "Physical forensic source changed since the frozen scan; refusing RW lease";
            return result;
        }

        ExpectedHeader item;
        item.lba = patch.lba;
        std::string patch_error;
        if (!forensic::build_patched_header(scan, patch, item.bytes, patch_error)) {
            result.error = "Could not freeze expected forensic APA header: " + patch_error;
            return result;
        }
        expected.push_back(std::move(item));
    }

    {
        WritablePhysicalDriveOptions writable_options;
        writable_options.authorization = authorization.authorization;
        writable_options.lock_mounted_volumes = options.lock_mounted_volumes;
        writable_options.dismount_locked_volumes = options.dismount_locked_volumes;

        WritablePhysicalDrive writable(physical_drive_index, writable_options);
        if (!writable.is_open()) {
            result.error = "Could not acquire guarded exceptional physical recovery lease: " +
                           writable.admission_error();
            return result;
        }
        result.locked_volume_count = writable.locked_volume_count();
        result.repair = recovery::repair_forensic_topology(
            writable, scan, plan, options.artifact_directory, allow_manual);
        if (!result.repair.ok) {
            result.partial = result.repair.partial;
            result.error = "Physical forensic APA repair failed: " + result.repair.error;
            return result;
        }
    }

    PhysicalDrive cold(physical_drive_index);
    if (!cold.is_open()) {
        result.partial = true;
        result.error = "Forensic APA repair completed, but cold read-only PhysicalDrive reopen failed";
        return result;
    }
    for (const auto& item : expected) {
        std::array<std::byte, forensic::kApaHeaderBytes> actual{};
        if (!read_header(cold, item.lba, actual) || actual != item.bytes) {
            result.partial = true;
            result.error = "Forensic APA repair completed, but cold touched-header verification failed";
            return result;
        }
    }
    result.cold_touched_set_verified = true;

    // A successful topology plan normally makes the canonical reader happy, but
    // leave the verdict explicit. If unrelated corruption remains, the exact
    // planner-approved touched set is still verified and the UI can direct the
    // operator back to forensic inspection rather than pretending the disk is
    // universally healthy.
    apa::Reader reader(cold);
    result.cold_apa_clean = reader.scan().ok();
    result.ok = true;
    return result;
}

} // namespace ps2hdd

#endif
