#include "ps2hdd/recovery_service.hpp"

#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/apa_recovery_apply.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/fhdb_rescue_capture.hpp"
#include "ps2hdd/fhdb_restore.hpp"
#include "ps2hdd/forensic_preview.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/writable_file_block_device.hpp"

#ifdef _WIN32
#include "ps2hdd/physical_apa_recovery.hpp"
#include "ps2hdd/physical_bootstrap_recovery.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_migration.hpp"
#endif

#include <cstdio>
#include <memory>

namespace ps2hdd::recovery_service {
namespace {

[[nodiscard]] std::unique_ptr<BlockDevice> open_read_only(const Source& source, std::string& error)
{
#ifdef _WIN32
    if (source.physical) {
        auto disk = std::make_unique<PhysicalDrive>(*source.physical);
        if (!disk->is_open()) {
            error = "Could not open PhysicalDrive" + std::to_string(*source.physical) + " read-only";
            return nullptr;
        }
        return disk;
    }
#endif
    if (source.physical) {
        error = "Physical drives are only supported on Windows";
        return nullptr;
    }
    auto image = std::make_unique<FileBlockDevice>(source.image);
    if (!image->is_open()) {
        error = "Could not open " + source.image.string();
        return nullptr;
    }
    return image;
}

[[nodiscard]] bool open_image_writable(const Source& source, std::unique_ptr<WritableFileBlockDevice>& out,
                                       Outcome& result)
{
    out = std::make_unique<WritableFileBlockDevice>(source.image);
    if (!out->is_open()) {
        result.error = "Could not open " + source.image.string() + " for writing (is it mounted?)";
        return false;
    }
    return true;
}

void absorb(Outcome& result, const recovery::RepairResult& repair)
{
    result.ok = repair.ok;
    result.partial = repair.partial;
    result.error = repair.error;
    if (!repair.snapshot_path.empty()) result.artifacts.push_back(repair.snapshot_path);
    if (!repair.forensic_report_path.empty()) result.artifacts.push_back(repair.forensic_report_path);
}

struct BestMap {
    bool found{};
    std::size_t index{};
    forensic::RepairPlan plan;
};

[[nodiscard]] BestMap best_automatic_map(const forensic::ScanResult& scan)
{
    BestMap best;
    for (std::size_t i = 0; i < scan.maps.size(); ++i) {
        if (!scan.maps[i].repairable) continue;
        auto plan = forensic::build_repair_plan(scan, i);
        if (!plan.automatic_safe) continue;
        if (!best.found || plan.confidence > best.plan.confidence) {
            best.found = true;
            best.index = i;
            best.plan = std::move(plan);
        }
    }
    return best;
}

[[nodiscard]] bool need(const Source& source, const std::filesystem::path& directory, const char* what,
                        Outcome& result)
{
    if (!source.valid()) {
        result.error = "Open a PS2 HDD or disk image first";
        return false;
    }
    if (directory.empty()) {
        result.error = std::string(what) + " requires a host-side safety/artifact directory";
        return false;
    }
    return true;
}

} // namespace

std::string Source::name() const
{
    if (physical) return "PhysicalDrive" + std::to_string(*physical);
    return image.filename().string();
}

HealthOutcome assess(const Source& source)
{
    HealthOutcome result;
    auto device = open_read_only(source, result.error);
    if (!device) return result;
    result.report = health::assess(*device);
    result.text = health::render(result.report);
    result.ok = true;
    return result;
}

BootChainOutcome boot_chain(const Source& source, const std::filesystem::path& memory_card_root)
{
    BootChainOutcome result;
    auto device = open_read_only(source, result.error);
    if (!device) return result;
    bootchain::Options options;
    options.memory_card_root = memory_card_root;
    result.report = bootchain::inspect(*device, options);
    result.text = bootchain::render(result.report);
    result.ok = result.report.ok;
    if (!result.ok) result.error = result.report.error;
    return result;
}

Outcome capture_rescue(const Source& source, const std::filesystem::path& artifact_directory)
{
    Outcome result;
    if (!need(source, artifact_directory, "Rescue Capsule capture", result)) return result;
    auto device = open_read_only(source, result.error);
    if (!device) return result;
    const auto rescue = fhdb::capture_rescue_image(*device);
    if (!rescue.ok) {
        result.error = rescue.error;
        return result;
    }
    const auto saved = fhdb::save_hddrescue(artifact_directory, rescue);
    if (!saved.ok) {
        result.error = saved.error;
        return result;
    }
    result.ok = true;
    result.artifacts.push_back(saved.path);
    result.summary = rescue.payload.empty() ? "Header-only Rescue Capsule saved (bootstrap is disabled)"
                     : (rescue.info.flags & fhdb::kRescueFlagValidKelf) != 0
                         ? "Rescue Capsule saved with a valid KELF payload"
                         : "Rescue Capsule saved; the payload is NOT a valid KELF, so it cannot be restored";
    return result;
}

Outcome restore_bootstrap(const Source& source, const std::filesystem::path& artifact_directory,
                          const std::filesystem::path& safety_directory)
{
    Outcome result;
    if (!need(source, safety_directory, "Bootstrap restore", result)) return result;
    if (artifact_directory.empty()) {
        result.error = "Choose the directory that holds HDDRESCUE/HDDMBR/FHDBMBR";
        return result;
    }
#ifdef _WIN32
    if (source.physical) {
        PhysicalBootstrapRestoreOptions options;
        options.artifact_directory = artifact_directory;
        options.safety_directory = safety_directory;
        const auto restored = restore_bootstrap_to_physical(*source.physical, options);
        result.ok = restored.ok;
        result.partial = restored.partial;
        result.error = restored.error;
        result.warning = restored.restore.warning;
        result.cold_verified = restored.cold_master_verified;
        if (!restored.restore.safety_backup_path.empty()) result.artifacts.push_back(restored.restore.safety_backup_path);
        result.summary = restored.ok ? "Bootstrap restored payload-first and cold-verified" : "Bootstrap restore did not complete";
        return result;
    }
#endif
    std::unique_ptr<WritableFileBlockDevice> disk;
    if (!open_image_writable(source, disk, result)) return result;
    const auto restored = fhdb::restore_bootstrap_from_directory_to_image(*disk, artifact_directory, safety_directory);
    result.ok = restored.ok;
    result.partial = restored.partial;
    result.error = restored.error;
    result.warning = restored.warning;
    result.cold_verified = restored.final_apa_verified;
    if (!restored.safety_backup_path.empty()) result.artifacts.push_back(restored.safety_backup_path);
    result.summary = restored.ok ? "Bootstrap restored from " + restored.source_path.filename().string()
                                 : "Bootstrap restore did not complete";
    return result;
}

Outcome disable_bootstrap(const Source& source, const std::filesystem::path& safety_directory)
{
    Outcome result;
    if (!need(source, safety_directory, "Disabling the bootstrap", result)) return result;
    fhdb::BootstrapDisableResult disabled;
#ifdef _WIN32
    if (source.physical) {
        const auto physical = disable_bootstrap_on_physical(*source.physical, safety_directory);
        disabled = physical.disable;
        result.ok = physical.ok;
        result.partial = physical.partial;
        result.error = physical.error;
        result.cold_verified = physical.cold_master_verified;
    }
#endif
    if (!source.physical) {
        std::unique_ptr<WritableFileBlockDevice> disk;
        if (!open_image_writable(source, disk, result)) return result;
        disabled = fhdb::disable_bootstrap(*disk, safety_directory);
        result.ok = disabled.ok;
        result.partial = disabled.partial;
        result.error = disabled.error;
        result.cold_verified = disabled.final_apa_verified;
    }
    result.warning = disabled.warning;
    result.nothing_to_do = disabled.already_disabled;
    if (!disabled.hddmbr_path.empty()) result.artifacts.push_back(disabled.hddmbr_path);
    if (!disabled.rescue_path.empty()) result.artifacts.push_back(disabled.rescue_path);
    result.summary = disabled.already_disabled ? "Bootstrap pointer was already disabled; nothing was written"
                     : result.ok ? "Bootstrap pointer cleared; partitions and games untouched"
                                 : "Bootstrap disable did not complete";
    return result;
}

Outcome repair_master(const Source& source, const std::filesystem::path& artifact_directory)
{
    Outcome result;
    if (!need(source, artifact_directory, "APA master repair", result)) return result;
#ifdef _WIN32
    if (source.physical) {
        PhysicalApaRecoveryOptions options;
        options.artifact_directory = artifact_directory;
        const auto repaired = repair_master_header_on_physical(*source.physical, options);
        absorb(result, repaired.repair);
        result.ok = repaired.ok;
        result.partial = repaired.partial;
        result.error = repaired.error;
        result.cold_verified = repaired.cold_master_verified;
        result.summary = repaired.ok ? (repaired.cold_apa_clean ? "APA master repaired; full chain is clean"
                                                                : "APA master repaired; other damage remains")
                                     : "APA master repair did not complete";
        return result;
    }
#endif
    std::unique_ptr<WritableFileBlockDevice> disk;
    if (!open_image_writable(source, disk, result)) return result;
    absorb(result, recovery::repair_master_header(*disk, artifact_directory));
    result.cold_verified = result.ok;
    result.summary = result.ok ? "APA master repaired (canonical fields + checksum)" : "APA master repair did not complete";
    return result;
}

Outcome forensic_report(const Source& source, const std::filesystem::path& artifact_directory)
{
    Outcome result;
    if (!need(source, artifact_directory, "Forensic analysis", result)) return result;
    auto device = open_read_only(source, result.error);
    if (!device) return result;
    const auto scan = forensic::scan_apa(*device);
    if (!scan.ok) {
        result.error = scan.error;
        return result;
    }
    const auto report = fhdb::save_forensic_report(artifact_directory, scan);
    if (!report.ok) {
        result.error = report.error;
        return result;
    }
    result.artifacts.push_back(report.path);
    for (std::size_t i = 0; i < scan.maps.size(); ++i)
        result.details += forensic::render_preview(forensic::preview(scan, i));
    const auto best = best_automatic_map(scan);
    if (best.found) {
        const auto meta = fhdb::save_hddmeta(artifact_directory, scan, best.plan);
        if (!meta.ok) {
            result.error = meta.error;
            return result;
        }
        result.artifacts.push_back(meta.path);
    }
    result.ok = true;
    result.summary = std::to_string(scan.nodes.size()) + " header(s), " + std::to_string(scan.maps.size()) +
                     " candidate map(s); " +
                     (best.found ? "map " + std::to_string(best.index) + " is automatic-safe (confidence " +
                                       std::to_string(best.plan.confidence) + ", " +
                                       std::to_string(best.plan.patches.size()) + " header patch(es))"
                                 : std::string("no automatic-safe map; no write is offered"));
    return result;
}

Outcome repair_forensic_automatic(const Source& source, const std::filesystem::path& artifact_directory)
{
    Outcome result;
    if (!need(source, artifact_directory, "Forensic topology repair", result)) return result;
    std::unique_ptr<BlockDevice> reader;
    std::unique_ptr<WritableFileBlockDevice> image;
    BlockDevice* scan_device = nullptr;
    if (source.physical) {
        reader = open_read_only(source, result.error);
        if (!reader) return result;
        scan_device = reader.get();
    } else {
        // Plan from the very handle that writes, so nothing can change between.
        if (!open_image_writable(source, image, result)) return result;
        scan_device = image.get();
    }
    const auto scan = forensic::scan_apa(*scan_device);
    if (!scan.ok) {
        result.error = scan.error;
        return result;
    }
    const auto best = best_automatic_map(scan);
    if (!best.found) {
        result.error = "No automatic-safe forensic topology repair is available; manual-only maps are not writable from the GUI";
        return result;
    }
#ifdef _WIN32
    if (source.physical) {
        reader.reset();
        PhysicalApaRecoveryOptions options;
        options.artifact_directory = artifact_directory;
        const auto repaired = repair_forensic_topology_on_physical(*source.physical, scan, best.plan, options, false);
        absorb(result, repaired.repair);
        result.ok = repaired.ok;
        result.partial = repaired.partial;
        result.error = repaired.error;
        result.cold_verified = repaired.cold_touched_set_verified && repaired.cold_apa_clean;
        result.summary = repaired.ok ? "Automatic-safe topology repair committed and cold-verified"
                                     : "Forensic topology repair did not complete";
        return result;
    }
#endif
    absorb(result, recovery::repair_forensic_topology(*image, scan, best.plan, artifact_directory, false));
    result.cold_verified = result.ok;
    result.summary = result.ok ? "Automatic-safe topology repair committed (map " + std::to_string(best.index) + ")"
                               : "Forensic topology repair did not complete";
    return result;
}

std::string human_bytes(std::uint64_t bytes)
{
    char text[64];
    const double value = static_cast<double>(bytes);
    if (value >= 1024.0 * 1024.0 * 1024.0) std::snprintf(text, sizeof(text), "%.2f GiB", value / (1024.0 * 1024.0 * 1024.0));
    else std::snprintf(text, sizeof(text), "%.0f MiB", value / (1024.0 * 1024.0));
    return text;
}

std::string MigrationTarget::name() const
{
    if (physical) return "PhysicalDrive" + std::to_string(*physical);
    return image.filename().string() + " (new image)";
}

MigrationPlanOutcome plan_migration(const Source& source, const MigrationTarget& target)
{
    MigrationPlanOutcome result;
    auto device = open_read_only(source, result.error);
    if (!device) return result;
    std::uint64_t target_bytes = 0;
    if (target.physical) {
#ifdef _WIN32
        // Use a permissive required size here; the plan below enforces it.
        if (!check_migration_target(*target.physical, source.physical, 0, target_bytes, result.error)) return result;
#else
        result.error = "Physical drives are only supported on Windows";
        return result;
#endif
    } else {
        if (target.image.empty() || target.image_bytes == 0) {
            result.error = "Choose a new image file and its size";
            return result;
        }
        std::error_code ec;
        if (std::filesystem::exists(target.image, ec)) {
            result.error = target.image.string() + " already exists; choose a new file name";
            return result;
        }
        if (!source.physical && std::filesystem::equivalent(source.image.parent_path() / source.image.filename(), target.image, ec)) {
            result.error = "The target is the source image";
            return result;
        }
        target_bytes = target.image_bytes;
    }
    result.plan = migrate::plan(*device, target_bytes);
    if (!result.plan.ok) {
        result.error = result.plan.error;
        return result;
    }
    const auto& plan = result.plan;
    result.text = source.name() + " -> " + target.name() + "\n" + std::to_string(plan.partitions) +
                  " partition(s), copying " + human_bytes(plan.copy_bytes) + " (of " + human_bytes(plan.source_bytes) +
                  " source)\nAllocated APA space ends at " + human_bytes(plan.required_bytes) + "; target " +
                  human_bytes(plan.target_bytes) + (plan.gained_bytes() >= 0 ? " (+" : " (") +
                  (plan.gained_bytes() >= 0 ? human_bytes(static_cast<std::uint64_t>(plan.gained_bytes()))
                                            : "-" + human_bytes(static_cast<std::uint64_t>(-plan.gained_bytes()))) +
                  " free space)";
    for (const auto& warning : plan.warnings) result.text += "\n[WARN] " + warning;
    result.ok = true;
    return result;
}

Outcome migrate(const Source& source, const MigrationTarget& target, const migrate::Plan& plan,
                migrate::Progress progress, std::stop_token stop)
{
    Outcome result;
    auto device = open_read_only(source, result.error);
    if (!device) return result;
    if (target.physical) {
#ifdef _WIN32
        const auto moved = migrate_to_physical(*device, source.physical, *target.physical, plan, std::move(progress), stop);
        result.ok = moved.ok;
        result.error = moved.error;
        result.cold_verified = moved.cold_verified;
        result.partial = !moved.ok && moved.copy.copied_bytes > 0;
        if (result.partial && !moved.copy.master_written)
            result.warning = "The target has no APA master yet, so the PS2 will not mistake it for a valid disk.";
#else
        result.error = "Physical drives are only supported on Windows";
        return result;
#endif
    } else {
        std::string error;
        if (!migrate::create_sparse_image(target.image, plan.target_bytes, error)) {
            result.error = error;
            return result;
        }
        WritableFileBlockDevice disk(target.image);
        if (!disk.is_open()) {
            result.error = "Could not open the new image for writing";
            return result;
        }
        migrate::Options options;
        options.target_is_zeroed = true;
        const auto copied = migrate::apply(*device, disk, plan, options, std::move(progress), stop);
        result.ok = copied.ok;
        result.error = copied.error;
        result.cold_verified = copied.verified;
        if (!copied.ok) {
            std::error_code ec;
            std::filesystem::remove(target.image, ec);  // a half image is worthless; never leave one around
            result.warning = "The incomplete image was deleted.";
        } else {
            result.artifacts.push_back(target.image);
        }
    }
    result.summary = result.ok ? "Migrated " + std::to_string(plan.partitions) + " partition(s) (" +
                                     human_bytes(plan.copy_bytes) + ") to " + target.name() + ", verified"
                               : "Migration did not complete";
    return result;
}

} // namespace ps2hdd::recovery_service
