#include "ps2hdd/apa_recovery_apply.hpp"

#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/fhdb_rescue.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ps2hdd::recovery {
namespace {

constexpr std::size_t kApaHeaderBytes = repair::kApaHeaderBytes;
constexpr std::uint64_t kSectorBytes = 512;

[[nodiscard]] bool read_header(BlockDevice& device, std::uint32_t lba,
                               std::array<std::byte, kApaHeaderBytes>& header) noexcept
{
    const auto offset = static_cast<std::uint64_t>(lba) * kSectorBytes;
    return offset <= device.size_bytes() && header.size() <= device.size_bytes() - offset &&
           device.read(offset, header);
}

[[nodiscard]] bool exact_source_matches(BlockDevice& device,
                                        const forensic::Node& node) noexcept
{
    std::array<std::byte, kApaHeaderBytes> current{};
    return read_header(device, node.lba, current) && current == node.header;
}

} // namespace

RepairResult repair_master_header(WritableBlockDevice& device,
                                  const std::filesystem::path& artifact_directory)
{
    RepairResult result;
    std::array<std::byte, kApaHeaderBytes> source{};
    if (!read_header(device, 0, source)) {
        result.error = "Could not read exact APA master sectors 0-1";
        return result;
    }

    auto plan = repair::analyze_apa_master(source);
    if (!plan.header_patch_safe || plan.blockers != 0 || plan.safe_header_fixes == 0) {
        result.error = "APA master damage is not an unambiguous FHDB-safe canonical repair";
        return result;
    }

    const auto snapshot = fhdb::save_hddraw(artifact_directory, source);
    if (!snapshot.ok) {
        result.error = "Could not preserve canonical HDDRAW snapshot: " + snapshot.error;
        return result;
    }
    result.snapshot_path = snapshot.path;

    // The backup is now durable. Re-read the device and recompute the plan so a
    // concurrent/stale image change cannot inherit authorization from old bytes.
    std::array<std::byte, kApaHeaderBytes> current{};
    if (!read_header(device, 0, current) || current != source) {
        result.error = "APA master changed after HDDRAW snapshot; refusing repair";
        return result;
    }
    plan = repair::analyze_apa_master(current);
    if (!plan.header_patch_safe || plan.blockers != 0 || plan.safe_header_fixes == 0) {
        result.error = "APA master no longer matches the approved conservative repair plan";
        return result;
    }

    std::array<std::byte, kApaHeaderBytes> repaired{};
    std::string build_error;
    if (!repair::build_repaired_apa_master(current, plan, repaired, build_error)) {
        result.error = "Could not materialize repaired APA master: " + build_error;
        return result;
    }

    if (!device.write(0, repaired)) {
        result.error = "Could not write repaired APA master sectors 0-1";
        return result;
    }
    result.partial = true;
    result.writes_completed = 1;
    if (!device.flush()) {
        result.error = "APA master write completed but durable flush failed";
        return result;
    }

    std::array<std::byte, kApaHeaderBytes> readback{};
    if (!read_header(device, 0, readback) || readback != repaired ||
        !fhdb::is_standard_apa_master(readback)) {
        result.error = "APA master write completed but read-back/canonical validation failed";
        return result;
    }

    result.ok = true;
    result.partial = false;
    return result;
}

RepairResult repair_forensic_topology(WritableBlockDevice& device,
                                       const forensic::ScanResult& scan,
                                       const forensic::RepairPlan& plan,
                                       const std::filesystem::path& artifact_directory,
                                       bool allow_manual)
{
    RepairResult result;
    if (!scan.ok || scan.truncated || plan.map_index >= scan.maps.size() ||
        plan.patches.empty()) {
        result.error = "Forensic APA topology repair requires a complete non-empty scan/plan";
        return result;
    }
    if (!plan.automatic_safe && !(allow_manual && plan.manual_allowed)) {
        result.error = "Forensic APA repair plan is speculative or lacks explicit manual authorization";
        return result;
    }

    const auto report = fhdb::save_forensic_report(artifact_directory, scan);
    if (!report.ok) {
        result.error = "Could not preserve canonical FORENSIC.TXT before repair: " + report.error;
        return result;
    }
    result.forensic_report_path = report.path;

    const auto snapshot = fhdb::save_hddmeta(artifact_directory, scan, plan);
    if (!snapshot.ok) {
        result.error = "Could not preserve canonical HDDMETA snapshot before repair: " + snapshot.error;
        return result;
    }
    result.snapshot_path = snapshot.path;

    // Validate the complete touched set before the first mutation. This catches
    // stale plans before we enter the deliberately non-atomic multi-header path.
    for (const auto& patch : plan.patches) {
        if (patch.node_index >= scan.nodes.size() ||
            scan.nodes[patch.node_index].lba != patch.lba ||
            !exact_source_matches(device, scan.nodes[patch.node_index])) {
            result.error = "Forensic source header changed after scan/snapshot; refusing all writes";
            return result;
        }
    }

    std::vector<const forensic::Patch*> order;
    order.reserve(plan.patches.size());
    for (const auto& patch : plan.patches) {
        if (patch.lba != 0) order.push_back(&patch);
    }
    for (const auto& patch : plan.patches) {
        if (patch.lba == 0) order.push_back(&patch);
    }

    struct Expected {
        std::uint32_t lba{};
        std::array<std::byte, kApaHeaderBytes> bytes{};
    };
    std::vector<Expected> expected;
    expected.reserve(order.size());

    for (const auto* patch : order) {
        const auto& node = scan.nodes[patch->node_index];
        // FHDB policy repeats source stability immediately before every write,
        // even though we already checked the full set above.
        if (!exact_source_matches(device, node)) {
            result.error = "Forensic source header changed immediately before write";
            result.partial = result.writes_completed != 0;
            return result;
        }

        Expected item;
        item.lba = patch->lba;
        std::string patch_error;
        if (!forensic::build_patched_header(scan, *patch, item.bytes, patch_error)) {
            result.error = "Could not materialize forensic APA patch: " + patch_error;
            result.partial = result.writes_completed != 0;
            return result;
        }

        const auto offset = static_cast<std::uint64_t>(patch->lba) * kSectorBytes;
        if (!device.write(offset, item.bytes)) {
            result.error = "Could not write forensic APA header patch";
            result.partial = result.writes_completed != 0;
            return result;
        }
        ++result.writes_completed;
        result.partial = true;
        if (!device.flush()) {
            result.error = "Forensic APA header was written but flush failed";
            return result;
        }

        std::array<std::byte, kApaHeaderBytes> readback{};
        if (!read_header(device, patch->lba, readback) || readback != item.bytes) {
            result.error = "Forensic APA header write failed immediate byte-for-byte verification";
            return result;
        }
        expected.push_back(std::move(item));
    }

    // One final touched-set pass catches storage/controller surprises after the
    // last flush. We intentionally do not run the normal APA Reader here because
    // this path exists specifically for disks whose ordinary parser admission
    // was already broken; the next application-level step can rescan normally.
    for (const auto& item : expected) {
        std::array<std::byte, kApaHeaderBytes> readback{};
        if (!read_header(device, item.lba, readback) || readback != item.bytes) {
            result.error = "Forensic repair completed writes but final touched-set verification failed";
            return result;
        }
    }

    result.ok = true;
    result.partial = false;
    return result;
}

} // namespace ps2hdd::recovery
