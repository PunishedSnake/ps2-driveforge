#include "ps2hdd/apa.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/version.hpp"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
#include "ps2hdd/physical_drive.hpp"
#endif

#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
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
                 "  ps2-driveforge-inspect <disk-image>\n"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
                 "  ps2-driveforge-inspect --physical <index>\n"
#endif
                 "\nRead-only: this build never writes to the source device.\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }

    if (std::string_view(argv[1]) == "--version") {
        std::cout << "PS2 DriveForge " << ps2hdd::version::string << "-dev ("
                  << ps2hdd::version::codename << ")\n";
        return 0;
    }

    std::unique_ptr<ps2hdd::BlockDevice> device;

#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
    if (std::string_view(argv[1]) == "--physical") {
        if (argc != 3) {
            usage();
            return 2;
        }
        try {
            auto physical = std::make_unique<ps2hdd::PhysicalDrive>(static_cast<unsigned>(std::stoul(argv[2])));
            if (!physical->is_open()) {
                std::cerr << "Could not open physical drive for read-only access. Run elevated if required.\n";
                return 1;
            }
            device = std::move(physical);
        } catch (...) {
            std::cerr << "Invalid physical drive index.\n";
            return 2;
        }
    } else
#endif
    {
        auto file = std::make_unique<ps2hdd::FileBlockDevice>(argv[1]);
        if (!file->is_open()) {
            std::cerr << "Could not open image: " << argv[1] << '\n';
            return 1;
        }
        device = std::move(file);
    }

    ps2hdd::apa::Reader reader(*device);
    const auto result = reader.scan();

    std::cout << "Source: " << device->display_name() << '\n'
              << "Size:   " << format_bytes(device->size_bytes()) << '\n'
              << "APA:    " << (result.mbr_valid ? "detected" : "not detected") << '\n';
    if (result.mbr_valid) {
        std::cout << "Version: " << result.apa_version << "\n\n";
        std::cout << std::left << std::setw(34) << "Partition"
                  << std::setw(12) << "Type"
                  << std::setw(16) << "Start LBA"
                  << std::setw(16) << "Main size"
                  << std::setw(12) << "Subs"
                  << "Total\n";
        std::cout << std::string(102, '-') << '\n';

        for (const auto& p : result.partitions) {
            std::cout << std::left << std::setw(34) << (p.id.empty() ? "<unnamed>" : p.id)
                      << std::setw(12) << ps2hdd::apa::type_name(p.type)
                      << std::setw(16) << p.start_lba
                      << std::setw(16) << format_bytes(static_cast<std::uint64_t>(p.length_sectors) * ps2hdd::apa::kSectorSize)
                      << std::setw(12) << p.sub_count
                      << format_bytes(p.size_bytes())
                      << (p.is_sub() ? "  [SUB]" : "") << '\n';

            if (p.type == ps2hdd::apa::kTypePfs && !p.is_sub()) {
                ps2hdd::ApaVolume volume(*device, p);
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
    }

    if (!result.issues.empty()) {
        std::cout << "\nDiagnostics:\n";
        for (const auto& issue : result.issues) {
            std::cout << "  [" << (issue.severity == ps2hdd::apa::IssueSeverity::error ? "ERROR" : "WARN")
                      << "] LBA " << issue.lba << ": " << issue.message << '\n';
        }
    }

    return result.ok() ? 0 : 1;
}
