#include "ps2hdd/apa.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/version.hpp"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
#include "ps2hdd/physical_drive.hpp"
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
                 "  ps2-driveforge-inspect <disk-image> --browse <partition> [path]\n"
                 "  ps2-driveforge-inspect <disk-image> --extract <partition> <pfs-path> <output>\n"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
                 "  ps2-driveforge-inspect --physical <index>\n"
                 "  ps2-driveforge-inspect --physical <index> --browse <partition> [path]\n"
                 "  ps2-driveforge-inspect --physical <index> --extract <partition> <pfs-path> <output>\n"
#endif
                 "  ps2-driveforge-inspect --version\n\n"
                 "Examples:\n"
                 "  ps2-driveforge-inspect ps2.img --browse +OPL\n"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
                 "  ps2-driveforge-inspect --physical 3 --browse +OPL CFG\n"
                 "  ps2-driveforge-inspect --physical 3 --extract +OPL CFG/SLUS_123.45.cfg game.cfg\n"
#endif
                 "\nSource access is read-only. --extract writes only to the requested host output file.\n";
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

const ps2hdd::apa::Partition* find_partition(const ps2hdd::apa::ScanResult& scan,
                                             std::string_view id)
{
    const auto it = std::find_if(scan.partitions.begin(), scan.partitions.end(),
                                 [&](const ps2hdd::apa::Partition& p) {
                                     return !p.is_sub() && p.id == id;
                                 });
    return it == scan.partitions.end() ? nullptr : &*it;
}

bool browse_pfs(ps2hdd::BlockDevice& device, const ps2hdd::apa::ScanResult& scan,
                const BrowseRequest& request)
{
    const auto* partition = find_partition(scan, request.partition);
    if (!partition) {
        std::cerr << "Partition not found: " << request.partition << '\n';
        return false;
    }
    if (partition->type != ps2hdd::apa::kTypePfs) {
        std::cerr << "Partition is not PFS: " << request.partition << '\n';
        return false;
    }

    ps2hdd::ApaVolume volume(device, *partition);
    ps2hdd::pfs::Reader pfs(volume);
    if (!pfs.valid()) {
        std::cerr << "Could not mount PFS read-only";
        if (!pfs.last_error().empty()) {
            std::cerr << ": " << pfs.last_error();
        }
        std::cerr << '\n';
        return false;
    }

    auto node = pfs.resolve(request.path);
    if (!node) {
        std::cerr << "Could not resolve PFS path";
        if (!pfs.last_error().empty()) {
            std::cerr << ": " << pfs.last_error();
        }
        std::cerr << '\n';
        return false;
    }
    if ((node->inode.mode & ps2hdd::pfs::kModeMask) != ps2hdd::pfs::kModeDirectory) {
        std::cerr << "PFS path is not a directory: " << request.path << '\n';
        return false;
    }

    const auto entries = pfs.list_directory(*node);
    if (!pfs.last_error().empty()) {
        std::cerr << "Could not enumerate PFS directory: " << pfs.last_error() << '\n';
        return false;
    }

    std::cout << "\nPFS browse: " << request.partition << ":/" << request.path << '\n'
              << std::left << std::setw(8) << "Type"
              << std::setw(14) << "Size"
              << std::setw(10) << "Sub"
              << std::setw(14) << "Inode"
              << "Name\n"
              << std::string(78, '-') << '\n';

    for (const auto& entry : entries) {
        auto child = pfs.read_inode(entry.inode);
        std::string type = entry.is_directory() ? "DIR" : (entry.is_regular() ? "FILE" : "OTHER");
        std::string size = "?";
        if (child) {
            size = format_bytes(child->inode.size);
            if ((child->inode.mode & ps2hdd::pfs::kModeMask) == ps2hdd::pfs::kModeDirectory) {
                type = "DIR";
            } else if ((child->inode.mode & ps2hdd::pfs::kModeMask) == ps2hdd::pfs::kModeRegular) {
                type = "FILE";
            }
        }
        std::cout << std::left << std::setw(8) << type
                  << std::setw(14) << size
                  << std::setw(10) << entry.inode.subpart
                  << std::setw(14) << entry.inode.number
                  << entry.name << '\n';
    }
    std::cout << entries.size() << " entr" << (entries.size() == 1 ? "y" : "ies") << "\n";
    return true;
}

bool extract_pfs(ps2hdd::BlockDevice& device, const ps2hdd::apa::ScanResult& scan,
                 const ExtractRequest& request)
{
    const auto* partition = find_partition(scan, request.partition);
    if (!partition) {
        std::cerr << "Partition not found: " << request.partition << '\n';
        return false;
    }
    if (partition->type != ps2hdd::apa::kTypePfs) {
        std::cerr << "Partition is not PFS: " << request.partition << '\n';
        return false;
    }

    ps2hdd::ApaVolume volume(device, *partition);
    ps2hdd::pfs::Reader pfs(volume);
    if (!pfs.valid()) {
        std::cerr << "Could not mount PFS read-only";
        if (!pfs.last_error().empty()) {
            std::cerr << ": " << pfs.last_error();
        }
        std::cerr << '\n';
        return false;
    }

    auto node = pfs.resolve(request.path);
    if (!node) {
        std::cerr << "Could not resolve PFS path: " << pfs.last_error() << '\n';
        return false;
    }
    if ((node->inode.mode & ps2hdd::pfs::kModeMask) == ps2hdd::pfs::kModeDirectory) {
        std::cerr << "PFS path is a directory; recursive extraction is not implemented yet.\n";
        return false;
    }

    std::ofstream output(request.output, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Could not create output file: " << request.output.string() << '\n';
        return false;
    }

    constexpr std::size_t kChunkSize = 1024 * 1024;
    std::vector<std::byte> buffer(kChunkSize);
    std::uint64_t offset = 0;
    while (offset < node->inode.size) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), node->inode.size - offset));
        if (!pfs.read(*node, offset, std::span<std::byte>(buffer.data(), chunk))) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(request.output, ignored);
            std::cerr << "PFS read failed at offset " << offset << ": " << pfs.last_error() << '\n';
            return false;
        }
        output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(chunk));
        if (!output) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(request.output, ignored);
            std::cerr << "Host write failed while extracting to: " << request.output.string() << '\n';
            return false;
        }
        offset += chunk;
    }

    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(request.output, ignored);
        std::cerr << "Could not finalize output file: " << request.output.string() << '\n';
        return false;
    }

    std::cout << "\nExtracted " << request.partition << ":/" << request.path
              << " -> " << request.output.string()
              << " (" << format_bytes(node->inode.size) << ")\n";
    return true;
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
    int next_arg = 2;

#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
    if (std::string_view(argv[1]) == "--physical") {
        if (argc < 3) {
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
            next_arg = 3;
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

    std::optional<BrowseRequest> browse;
    std::optional<ExtractRequest> extract;
    if (next_arg < argc) {
        const std::string_view command = argv[next_arg];
        if (command == "--browse") {
            if (next_arg + 1 >= argc || next_arg + 3 < argc) {
                usage();
                return 2;
            }
            BrowseRequest request;
            request.partition = argv[next_arg + 1];
            if (next_arg + 2 < argc) {
                request.path = argv[next_arg + 2];
            }
            browse = std::move(request);
        } else if (command == "--extract") {
            if (next_arg + 3 >= argc || next_arg + 4 < argc) {
                usage();
                return 2;
            }
            extract = ExtractRequest{argv[next_arg + 1], argv[next_arg + 2], argv[next_arg + 3]};
        } else {
            usage();
            return 2;
        }
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

    if (!result.ok() && (browse || extract)) {
        std::cerr << "Refusing PFS operation because APA diagnostics contain fatal errors.\n";
        return 1;
    }
    if (browse) {
        return browse_pfs(*device, result, *browse) ? 0 : 1;
    }
    if (extract) {
        return extract_pfs(*device, result, *extract) ? 0 : 1;
    }

    return result.ok() ? 0 : 1;
}
