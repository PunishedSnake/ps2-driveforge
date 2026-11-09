// Disk-image structure health and FHDB-parity recovery. Images are where a
// repair should be rehearsed before it ever meets a PhysicalDrive, so every
// recovery path the physical tool has is available here too, on the same core
// writers. PhysicalDrive access lives in ps2-driveforge-physical-tools.

#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/boot_chain.hpp"
#include "ps2hdd/apa_recovery_apply.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/fhdb_rescue_capture.hpp"
#include "ps2hdd/fhdb_restore.hpp"
#include "ps2hdd/forensic_preview.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/recovery_service.hpp"
#include "ps2hdd/structure_health.hpp"
#include "ps2hdd/version.hpp"
#include "ps2hdd/writable_file_block_device.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace {

void usage()
{
    std::cerr
        << "ps2-driveforge-repair-tools " << ps2hdd::version::string << "\n\n"
        << "Read-only:\n"
        << "  ps2-driveforge-repair-tools health <disk-image>\n"
        << "  ps2-driveforge-repair-tools boot-chain <disk-image> [--mc <memory-card-folder>]\n"
        << "  ps2-driveforge-repair-tools forensic-scan <disk-image> [<artifact-dir>]\n"
        << "  ps2-driveforge-repair-tools capture-rescue <disk-image> <artifact-dir>\n\n"
        << "Image mutation (explicit --apply required; safety artifacts are written first):\n"
        << "  ps2-driveforge-repair-tools repair-master <disk-image> <artifact-dir> --apply\n"
        << "  ps2-driveforge-repair-tools repair-forensic <disk-image> <map-index> <artifact-dir> --apply\n"
        << "      [--allow-manual]\n"
        << "  ps2-driveforge-repair-tools disable-bootstrap <disk-image> <safety-dir> --apply\n"
        << "  ps2-driveforge-repair-tools restore-bootstrap <disk-image> <artifact-dir> <safety-dir> --apply\n\n"
        << "Migration (copies only APA-allocated space; the new image is sparse):\n"
        << "  ps2-driveforge-repair-tools migrate <disk-image> <new-image> <size e.g. 500G> [--apply]\n\n"
        << "health exits 0 when healthy, 4 otherwise. Artifacts use the FHDB Manager\n"
        << "names (HDDMBR/HDDRAW/HDDMETA/HDDRESCUE/FORENSIC.TXT) and interoperate with it.\n";
}

bool has_flag(std::span<char*> args, std::string_view flag)
{
    return std::any_of(args.begin(), args.end(), [flag](const char* arg) { return flag == arg; });
}

int need_apply(std::span<char*> args)
{
    if (has_flag(args, "--apply")) return 0;
    std::cerr << "This command modifies the image; add --apply after reviewing `health`.\n";
    return 2;
}

int report_repair(const char* what, const ps2hdd::recovery::RepairResult& result)
{
    if (!result.snapshot_path.empty()) std::cout << "  snapshot: " << result.snapshot_path.string() << '\n';
    if (!result.forensic_report_path.empty()) std::cout << "  FORENSIC: " << result.forensic_report_path.string() << '\n';
    if (!result.ok) {
        std::cerr << what << " failed" << (result.partial ? " (partially applied)" : "") << ": " << result.error << '\n';
        return result.partial ? 3 : 1;
    }
    std::cout << what << " complete (" << result.writes_completed << " header write(s))\n";
    return 0;
}


bool parse_size(std::string_view text, std::uint64_t& bytes)
{
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || value == 0) return false;
    const std::string_view unit(end, static_cast<std::size_t>(text.data() + text.size() - end));
    std::uint64_t scale = 1;
    if (unit == "K" || unit == "KiB") scale = 1ULL << 10;
    else if (unit == "M" || unit == "MiB") scale = 1ULL << 20;
    else if (unit == "G" || unit == "GiB") scale = 1ULL << 30;
    else if (unit == "T" || unit == "TiB") scale = 1ULL << 40;
    else if (!unit.empty()) return false;
    bytes = value * scale;
    return true;
}

int run_migration(const ps2hdd::recovery_service::Source& source, const ps2hdd::recovery_service::MigrationTarget& target,
                  bool apply)
{
    namespace rs = ps2hdd::recovery_service;
    const auto planned = rs::plan_migration(source, target);
    if (!planned.ok) {
        std::cerr << "Migration refused: " << planned.error << '\n';
        return 1;
    }
    std::cout << planned.text << '\n';
    if (!apply) {
        std::cout << "Dry run; add --apply to copy.\n";
        return 0;
    }
    int last = -1;
    const auto outcome = rs::migrate(source, target, planned.plan, [&last](std::uint64_t done, std::uint64_t total) {
        const int pct = total == 0 ? 100 : static_cast<int>(done * 100U / total);
        if (pct != last) {
            last = pct;
            std::cerr << "\r  copying " << pct << "%" << std::flush;
        }
    });
    std::cerr << '\n';
    if (!outcome.warning.empty()) std::cout << "[WARN] " << outcome.warning << '\n';
    if (!outcome.ok) {
        std::cerr << "Migration failed: " << outcome.error << '\n';
        return outcome.partial ? 3 : 1;
    }
    std::cout << outcome.summary << '\n';
    return 0;
}

int health(const std::filesystem::path& image)
{
    ps2hdd::FileBlockDevice disk(image);
    if (!disk.is_open()) {
        std::cerr << "Could not open " << image.string() << '\n';
        return 1;
    }
    const auto report = ps2hdd::health::assess(disk);
    std::cout << ps2hdd::health::render(report);
    return report.verdict == ps2hdd::health::Verdict::healthy ? 0 : 4;
}

int forensic_scan(const std::filesystem::path& image, const std::filesystem::path& artifacts)
{
    ps2hdd::FileBlockDevice disk(image);
    if (!disk.is_open()) {
        std::cerr << "Could not open " << image.string() << '\n';
        return 1;
    }
    const auto scan = ps2hdd::forensic::scan_apa(disk);
    std::cout << ps2hdd::forensic::render_report(scan);
    std::cout << '\n';
    for (std::size_t i = 0; i < scan.maps.size(); ++i)
        std::cout << ps2hdd::forensic::render_preview(ps2hdd::forensic::preview(scan, i)) << '\n';
    if (!artifacts.empty()) {
        const auto saved = ps2hdd::fhdb::save_forensic_report(artifacts, scan);
        if (!saved.ok) {
            std::cerr << "Could not save FORENSIC.TXT: " << saved.error << '\n';
            return 1;
        }
        std::cout << "FORENSIC: " << saved.path.string() << '\n';
    }
    return scan.ok ? 0 : 1;
}

int capture_rescue(const std::filesystem::path& image, const std::filesystem::path& artifacts)
{
    ps2hdd::FileBlockDevice disk(image);
    if (!disk.is_open()) {
        std::cerr << "Could not open " << image.string() << '\n';
        return 1;
    }
    const auto rescue = ps2hdd::fhdb::capture_rescue_image(disk);
    if (!rescue.ok) {
        std::cerr << "Rescue Capsule capture refused: " << rescue.error << '\n';
        return 1;
    }
    const auto saved = ps2hdd::fhdb::save_hddrescue(artifacts, rescue);
    if (!saved.ok) {
        std::cerr << "Could not save the Rescue Capsule: " << saved.error << '\n';
        return 1;
    }
    std::cout << "Rescue Capsule: " << saved.path.string() << (saved.reused ? " (identical capsule reused)" : "")
              << "\n  payload: " << rescue.payload.size() << " byte(s), KELF "
              << ((rescue.info.flags & ps2hdd::fhdb::kRescueFlagValidKelf) != 0 ? "valid" : "not valid") << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string_view command = argv[1];
    if (command == "--version") {
        std::cout << ps2hdd::version::string << '\n';
        return 0;
    }
    if (command == "health" && argc == 3) return health(argv[2]);
    if (command == "migrate" && argc >= 5) {
        ps2hdd::recovery_service::Source source;
        source.image = argv[2];
        ps2hdd::recovery_service::MigrationTarget target;
        target.image = argv[3];
        if (!parse_size(argv[4], target.image_bytes)) {
            std::cerr << "Invalid size (use bytes or a K/M/G/T suffix)\n";
            return 2;
        }
        return run_migration(source, target, has_flag(args, "--apply"));
    }
    if (command == "boot-chain" && (argc == 3 || argc == 5)) {
        ps2hdd::FileBlockDevice disk(argv[2]);
        if (!disk.is_open()) {
            std::cerr << "Could not open " << argv[2] << '\n';
            return 1;
        }
        ps2hdd::bootchain::Options options;
        if (argc == 5 && std::string_view(argv[3]) == "--mc") options.memory_card_root = argv[4];
        const auto report = ps2hdd::bootchain::inspect(disk, options);
        std::cout << ps2hdd::bootchain::render(report);
        return report.ok ? 0 : 1;
    }
    if (command == "forensic-scan" && (argc == 3 || argc == 4))
        return forensic_scan(argv[2], argc == 4 ? std::filesystem::path(argv[3]) : std::filesystem::path{});
    if (command == "capture-rescue" && argc == 4) return capture_rescue(argv[2], argv[3]);

    if (command == "repair-master" && argc >= 4) {
        if (const int rc = need_apply(args)) return rc;
        ps2hdd::WritableFileBlockDevice disk(argv[2]);
        if (!disk.is_open()) {
            std::cerr << "Could not open " << argv[2] << " for writing\n";
            return 1;
        }
        return report_repair("APA master repair", ps2hdd::recovery::repair_master_header(disk, argv[3]));
    }
    if (command == "repair-forensic" && argc >= 5) {
        if (const int rc = need_apply(args)) return rc;
        std::size_t map_index = 0;
        const std::string_view text = argv[3];
        if (std::from_chars(text.data(), text.data() + text.size(), map_index).ec != std::errc{}) {
            std::cerr << "Invalid map index\n";
            return 2;
        }
        ps2hdd::WritableFileBlockDevice disk(argv[2]);
        if (!disk.is_open()) {
            std::cerr << "Could not open " << argv[2] << " for writing\n";
            return 1;
        }
        // Scan and plan from the same handle that will be written, immediately
        // before the write; the writer still re-reads every touched header.
        const auto scan = ps2hdd::forensic::scan_apa(disk);
        if (!scan.ok || map_index >= scan.maps.size()) {
            std::cerr << "Forensic scan failed or map index is out of range (" << scan.maps.size() << " map(s))\n";
            return 1;
        }
        const auto plan = ps2hdd::forensic::build_repair_plan(scan, map_index);
        const bool manual = has_flag(args, "--allow-manual");
        if (!plan.automatic_safe && !(manual && plan.manual_allowed)) {
            std::cerr << "Map " << map_index << " is not automatic-safe"
                      << (plan.manual_allowed ? "; rerun with --allow-manual after reviewing FORENSIC.TXT" : " and manual repair is blocked")
                      << '\n';
            return 1;
        }
        return report_repair("Forensic topology repair",
                             ps2hdd::recovery::repair_forensic_topology(disk, scan, plan, argv[4], manual));
    }
    if (command == "disable-bootstrap" && argc >= 4) {
        if (const int rc = need_apply(args)) return rc;
        ps2hdd::WritableFileBlockDevice disk(argv[2]);
        if (!disk.is_open()) {
            std::cerr << "Could not open " << argv[2] << " for writing\n";
            return 1;
        }
        const auto result = ps2hdd::fhdb::disable_bootstrap(disk, argv[3]);
        if (!result.hddmbr_path.empty()) std::cout << "  HDDMBR: " << result.hddmbr_path.string() << '\n';
        if (!result.rescue_path.empty()) std::cout << "  rescue: " << result.rescue_path.string() << '\n';
        if (!result.warning.empty()) std::cout << "[WARN] " << result.warning << '\n';
        if (!result.ok) {
            std::cerr << "Bootstrap disable failed: " << result.error << '\n';
            return result.partial ? 3 : 1;
        }
        std::cout << (result.already_disabled ? "Bootstrap pointer was already disabled; nothing written\n"
                                              : "Bootstrap pointer cleared and verified\n");
        return 0;
    }
    if (command == "restore-bootstrap" && argc >= 5) {
        if (const int rc = need_apply(args)) return rc;
        ps2hdd::WritableFileBlockDevice disk(argv[2]);
        if (!disk.is_open()) {
            std::cerr << "Could not open " << argv[2] << " for writing\n";
            return 1;
        }
        const auto result = ps2hdd::fhdb::restore_bootstrap_from_directory_to_image(disk, argv[3], argv[4]);
        if (!result.warning.empty()) std::cout << "[WARN] " << result.warning << '\n';
        if (!result.ok) {
            std::cerr << "Bootstrap restore failed: " << result.error << '\n';
            return result.partial ? 3 : 1;
        }
        std::cout << "Bootstrap restored from " << result.source_path.string() << " (pointer 0x" << std::hex
                  << result.payload_start << ", 0x" << result.payload_sectors << std::dec << ")\n"
                  << "  safety HDDMBR: " << result.safety_backup_path.string() << '\n';
        return 0;
    }
    usage();
    return 2;
}
