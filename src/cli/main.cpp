#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/version.hpp"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
#include "ps2hdd/physical_discovery.hpp"
#include "ps2hdd/physical_drive.hpp"
#endif

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

std::string format_bytes(std::uint64_t bytes)
{
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' ' << units[unit];
    return out.str();
}

void usage()
{
    std::cout << "PS2 DriveForge " << ps2hdd::version::string << "-dev \""
              << ps2hdd::version::codename << "\" - APA/PFS inspector\n\n"
                 "Usage:\n"
                 "  ps2-driveforge-inspect [--stats] <disk-image>\n"
                 "  ps2-driveforge-inspect [--stats] <disk-image> --browse <partition> [path]\n"
                 "  ps2-driveforge-inspect [--stats] <disk-image> --extract <partition> <pfs-path> <output>\n"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
                 "  ps2-driveforge-inspect [--stats] --physical <index>\n"
                 "  ps2-driveforge-inspect [--stats] --physical <index> --browse <partition> [path]\n"
                 "  ps2-driveforge-inspect [--stats] --physical <index> --extract <partition> <pfs-path> <output>\n"
                 "  ps2-driveforge-inspect --detect-physical [max-index]\n"
#endif
                 "  ps2-driveforge-inspect --version\n\n"
                 "Examples:\n"
                 "  ps2-driveforge-inspect --stats ps2.img --browse +OPL\n"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
                 "  ps2-driveforge-inspect --detect-physical\n"
                 "  ps2-driveforge-inspect --stats --physical 3 --browse +OPL CFG\n"
                 "  ps2-driveforge-inspect --stats --physical 3 --extract +OPL / exported-OPL\n"
#endif
                 "\nSource access is read-only. --extract may recursively create host files/directories.\n";
}

struct BrowseRequest {
    std::string partition;
    std::string path;
};

struct ExtractRequest {
    std::string partition;
    std::string path;
    std::filesystem::path output;
};

void print_stats(const ps2hdd::SessionStats& stats)
{
    const auto& io = stats.backing_io;
    const auto average = io.read_calls == 0 ? 0 : io.bytes_requested / io.read_calls;
    std::cout << "\nI/O statistics:\n"
              << "  APA scans:             " << stats.apa_scans << '\n'
              << "  PFS browse operations: " << stats.browse_operations << '\n'
              << "  PFS export operations: " << stats.export_operations << '\n'
              << "  Backing read calls:    " << io.read_calls << '\n'
              << "  Backing bytes read:    " << format_bytes(io.bytes_requested) << '\n'
              << "  Average backing read:  " << format_bytes(average) << '\n'
              << "  Largest backing read:  " << format_bytes(io.largest_read) << '\n'
              << "  Failed backing reads:  " << io.failed_reads << '\n';
}

#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
int detect_physical(unsigned max_index)
{
    const auto probes = ps2hdd::discover_physical_drives(max_index);
    std::cout << "Read-only Windows physical-drive discovery (0.." << (max_index == 0 ? 0 : max_index - 1) << ")\n\n";
    std::cout << std::left << std::setw(18) << "Drive"
              << std::setw(14) << "Size"
              << std::setw(16) << "APA"
              << std::setw(10) << "Version"
              << std::setw(12) << "Headers"
              << "Status\n"
              << std::string(82, '-') << '\n';

    std::size_t candidates = 0;
    for (const auto& probe : probes) {
        if (probe.apa_detected) {
            ++candidates;
        }
        std::cout << std::left << std::setw(18) << ("PhysicalDrive" + std::to_string(probe.index))
                  << std::setw(14) << format_bytes(probe.size_bytes)
                  << std::setw(16) << (probe.apa_detected ? (probe.apa_clean ? "PS2 APA" : "APA/errors") : "not APA")
                  << std::setw(10) << (probe.apa_detected ? std::to_string(probe.apa_version) : "-")
                  << std::setw(12) << probe.partition_count
                  << probe.note << '\n';
    }
    std::cout << "\nOpenable drives: " << probes.size() << ", PS2 APA candidates: " << candidates << '\n'
              << "Discovery requests GENERIC_READ only; it never enables source writes.\n";
    return candidates == 0 ? 1 : 0;
}
#endif

void print_overview(ps2hdd::DriveSession& session)
{
    const auto& result = session.scan_result();
    std::cout << "Source: " << session.device().display_name() << '\n'
              << "Size:   " << format_bytes(session.device().size_bytes()) << '\n'
              << "APA:    " << (result.mbr_valid ? "detected" : "not detected") << '\n';
    if (!result.mbr_valid) {
        return;
    }

    std::cout << "Version: " << result.apa_version << "\n\n"
              << std::left << std::setw(34) << "Partition"
              << std::setw(12) << "Type"
              << std::setw(16) << "Start LBA"
              << std::setw(16) << "Main size"
              << std::setw(12) << "Subs"
              << "Total\n"
              << std::string(102, '-') << '\n';

    for (const auto& p : result.partitions) {
        std::cout << std::left << std::setw(34) << (p.id.empty() ? "<unnamed>" : p.id)
                  << std::setw(12) << ps2hdd::apa::type_name(p.type)
                  << std::setw(16) << p.start_lba
                  << std::setw(16) << format_bytes(static_cast<std::uint64_t>(p.length_sectors) * ps2hdd::apa::kSectorSize)
                  << std::setw(12) << p.sub_count
                  << format_bytes(p.size_bytes())
                  << (p.is_sub() ? "  [SUB]" : "") << '\n';

        if (p.type == ps2hdd::apa::kTypePfs && !p.is_sub()) {
            ps2hdd::ApaVolume volume(session.device(), p);
            const auto pfs = ps2hdd::pfs::probe(volume);
            if (pfs.valid) {
                std::cout << "    PFS v" << pfs.super.version
                          << ", zone " << format_bytes(pfs.super.zone_size)
                          << ", filesystem subs " << pfs.super.num_subs
                          << ", backup " << (pfs.backup_matches ? "matches" : "differs/unreadable") << '\n';
                for (const auto& warning : pfs.warnings) {
                    std::cout << "      [PFS WARN] " << warning << '\n';
                }
            } else {
                for (const auto& error : pfs.errors) {
                    std::cout << "      [PFS ERROR] " << error << '\n';
                }
            }
        }
    }

    if (!result.issues.empty()) {
        std::cout << "\nDiagnostics:\n";
        for (const auto& issue : result.issues) {
            std::cout << "  [" << (issue.severity == ps2hdd::apa::IssueSeverity::error ? "ERROR" : "WARN")
                      << "] LBA " << issue.lba << ": " << issue.message << '\n';
        }
    }
}

bool browse(ps2hdd::DriveSession& session, const BrowseRequest& request)
{
    const auto result = session.browse(request.partition, request.path);
    if (!result.ok) {
        std::cerr << "PFS browse failed: " << result.error << '\n';
        return false;
    }

    std::cout << "\nPFS browse: " << request.partition << ":/" << request.path << '\n'
              << std::left << std::setw(8) << "Type"
              << std::setw(14) << "Size"
              << std::setw(10) << "Sub"
              << std::setw(14) << "Inode"
              << "Name\n"
              << std::string(78, '-') << '\n';

    for (const auto& entry : result.entries) {
        const std::string type = entry.is_directory() ? "DIR" : (entry.is_regular() ? "FILE" : "OTHER");
        std::cout << std::left << std::setw(8) << type
                  << std::setw(14) << (entry.inode_readable ? format_bytes(entry.size) : "?")
                  << std::setw(10) << entry.inode.subpart
                  << std::setw(14) << entry.inode.number
                  << entry.name << '\n';
    }
    std::cout << result.entries.size() << " entr" << (result.entries.size() == 1 ? "y" : "ies") << '\n';
    return true;
}

bool extract(ps2hdd::DriveSession& session, const ExtractRequest& request)
{
    std::uint64_t last_reported = 0;
    const auto result = session.export_to_host(
        request.partition, request.path, request.output,
        [&](const ps2hdd::pfs::ExportStats& stats, std::string_view current) {
            if (stats.bytes >= last_reported + 64ULL * 1024ULL * 1024ULL) {
                std::cout << "  " << format_bytes(stats.bytes) << " exported; current: " << current << '\n';
                last_reported = stats.bytes;
            }
        });

    if (!result.ok) {
        std::cerr << "PFS export failed: " << result.error << '\n';
        return false;
    }

    std::cout << "\nExported " << request.partition << ":/" << request.path
              << " -> " << request.output.string() << '\n'
              << "Files: " << result.stats.files
              << ", directories: " << result.stats.directories
              << ", bytes: " << format_bytes(result.stats.bytes)
              << ", skipped: " << result.stats.skipped << '\n';
    for (const auto& warning : result.warnings) {
        std::cout << "  [EXPORT WARN] " << warning << '\n';
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }

    int arg = 1;
    if (std::string_view(argv[arg]) == "--version") {
        std::cout << "PS2 DriveForge " << ps2hdd::version::string << "-dev ("
                  << ps2hdd::version::codename << ")\n";
        return 0;
    }

    bool show_stats = false;
    if (std::string_view(argv[arg]) == "--stats") {
        show_stats = true;
        if (++arg >= argc) {
            usage();
            return 2;
        }
    }

#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
    if (std::string_view(argv[arg]) == "--detect-physical") {
        unsigned max_index = 32;
        if (arg + 1 < argc) {
            try {
                max_index = static_cast<unsigned>(std::stoul(argv[arg + 1]));
            } catch (...) {
                std::cerr << "Invalid physical discovery limit.\n";
                return 2;
            }
        }
        return detect_physical(max_index);
    }
#endif

    std::unique_ptr<ps2hdd::BlockDevice> source;
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
    if (std::string_view(argv[arg]) == "--physical") {
        if (arg + 1 >= argc) {
            usage();
            return 2;
        }
        try {
            auto physical = std::make_unique<ps2hdd::PhysicalDrive>(static_cast<unsigned>(std::stoul(argv[arg + 1])));
            if (!physical->is_open()) {
                std::cerr << "Could not open physical drive for read-only access. Run elevated if required.\n";
                return 1;
            }
            source = std::move(physical);
            arg += 2;
        } catch (...) {
            std::cerr << "Invalid physical drive index.\n";
            return 2;
        }
    } else
#endif
    {
        auto file = std::make_unique<ps2hdd::FileBlockDevice>(argv[arg]);
        if (!file->is_open()) {
            std::cerr << "Could not open image: " << argv[arg] << '\n';
            return 1;
        }
        source = std::move(file);
        ++arg;
    }

    std::optional<BrowseRequest> browse_request;
    std::optional<ExtractRequest> extract_request;
    if (arg < argc) {
        const std::string_view command = argv[arg];
        if (command == "--browse") {
            if (arg + 1 >= argc || arg + 3 < argc) {
                usage();
                return 2;
            }
            BrowseRequest request;
            request.partition = argv[arg + 1];
            if (arg + 2 < argc) {
                request.path = argv[arg + 2];
            }
            browse_request = std::move(request);
        } else if (command == "--extract") {
            if (arg + 3 >= argc || arg + 4 < argc) {
                usage();
                return 2;
            }
            extract_request = ExtractRequest{argv[arg + 1], argv[arg + 2], argv[arg + 3]};
        } else {
            usage();
            return 2;
        }
    }

    ps2hdd::DriveSession session(std::move(source));
    const bool clean_scan = session.scan();
    print_overview(session);

    int exit_code = session.scan_result().ok() ? 0 : 1;
    if (!clean_scan && (browse_request || extract_request)) {
        std::cerr << "Refusing PFS operation because APA diagnostics contain fatal errors.\n";
    } else if (browse_request) {
        exit_code = browse(session, *browse_request) ? 0 : 1;
    } else if (extract_request) {
        exit_code = extract(session, *extract_request) ? 0 : 1;
    }

    if (show_stats) {
        print_stats(session.stats());
    }
    return exit_code;
}
