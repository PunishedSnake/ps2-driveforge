#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_filesystem.hpp"
#include "ps2hdd/version.hpp"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
#include "ps2hdd/physical_drive.hpp"
#endif

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class Action { scan, browse, tree, extract };

struct Options {
    Action action{Action::scan};
    std::string partition;
    std::string pfs_path{"/"};
    std::string host_path;
};

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
                 "  ps2-driveforge-inspect <disk-image> [action]\n"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
                 "  ps2-driveforge-inspect --physical <index> [action]\n"
#endif
                 "\nActions:\n"
                 "  --browse <partition> [path]              List one PFS directory\n"
                 "  --tree <partition> [path]                Recursively list a PFS tree\n"
                 "  --extract <partition> <path> <host-file> Extract one PFS file\n"
                 "\nExamples:\n"
                 "  ps2-driveforge-inspect --physical 3 --browse +OPL\n"
                 "  ps2-driveforge-inspect --physical 3 --tree +OPL /CFG\n"
                 "  ps2-driveforge-inspect --physical 3 --extract +OPL /CFG/game.cfg game.cfg\n"
                 "\nRead-only: this build never writes to the source device.\n";
}

bool parse_action(int argc, char** argv, int index, Options& options)
{
    if (index == argc) {
        return true;
    }
    const std::string_view action = argv[index++];
    if (action == "--browse" || action == "--tree") {
        if (index >= argc) {
            return false;
        }
        options.action = action == "--browse" ? Action::browse : Action::tree;
        options.partition = argv[index++];
        if (index < argc) {
            options.pfs_path = argv[index++];
        }
        return index == argc;
    }
    if (action == "--extract") {
        if (index + 2 >= argc) {
            return false;
        }
        options.action = Action::extract;
        options.partition = argv[index++];
        options.pfs_path = argv[index++];
        options.host_path = argv[index++];
        return index == argc;
    }
    return false;
}

const ps2hdd::apa::Partition* find_pfs_partition(const ps2hdd::apa::ScanResult& scan,
                                                  std::string_view id)
{
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.type == ps2hdd::apa::kTypePfs && partition.id == id) {
            return &partition;
        }
    }
    return nullptr;
}

std::string type_letter(std::uint16_t type)
{
    if (type == ps2hdd::pfs::kModeDirectory) return "D";
    if (type == ps2hdd::pfs::kModeRegular) return "F";
    if (type == ps2hdd::pfs::kModeSymlink) return "L";
    return "?";
}

bool print_directory(ps2hdd::pfs::FileSystem& fs, const ps2hdd::pfs::BlockInfo& location)
{
    const auto listing = fs.list_directory(location);
    if (!listing.ok()) {
        for (const auto& error : listing.errors) std::cerr << "PFS error: " << error << '\n';
        return false;
    }

    for (const auto& entry : listing.entries) {
        ps2hdd::pfs::Inode inode{};
        std::string error;
        const bool have_inode = fs.read_inode(entry.location, inode, &error);
        std::cout << type_letter(entry.type) << "  " << std::right << std::setw(12)
                  << (have_inode ? format_bytes(inode.size) : std::string("?"))
                  << "  " << entry.name;
        if (!have_inode) std::cout << "  [stat failed: " << error << ']';
        std::cout << '\n';
    }
    return true;
}

std::uint64_t node_key(const ps2hdd::pfs::BlockInfo& location)
{
    return (static_cast<std::uint64_t>(location.subpart) << 32U) | location.number;
}

bool print_tree(ps2hdd::pfs::FileSystem& fs, const ps2hdd::pfs::BlockInfo& location,
                std::string_view prefix, unsigned depth, std::set<std::uint64_t>& visited)
{
    if (depth > 64) {
        std::cerr << "PFS error: directory recursion limit reached\n";
        return false;
    }
    if (!visited.insert(node_key(location)).second) {
        std::cout << prefix << "[directory cycle]\n";
        return true;
    }

    const auto listing = fs.list_directory(location);
    if (!listing.ok()) {
        for (const auto& error : listing.errors) std::cerr << "PFS error: " << error << '\n';
        return false;
    }

    for (const auto& entry : listing.entries) {
        ps2hdd::pfs::Inode inode{};
        std::string error;
        const bool have_inode = fs.read_inode(entry.location, inode, &error);
        std::cout << prefix << (entry.is_directory() ? "[D] " : "[F] ") << entry.name;
        if (have_inode && !entry.is_directory()) std::cout << "  (" << format_bytes(inode.size) << ')';
        std::cout << '\n';

        if (entry.is_directory() && have_inode) {
            if (!print_tree(fs, entry.location, std::string(prefix) + "  ", depth + 1, visited)) return false;
        }
    }
    return true;
}

bool mount_selected_pfs(ps2hdd::BlockDevice& device, const ps2hdd::apa::ScanResult& scan,
                        const Options& options, std::unique_ptr<ps2hdd::ApaVolume>& volume,
                        std::unique_ptr<ps2hdd::pfs::FileSystem>& fs)
{
    const auto* partition = find_pfs_partition(scan, options.partition);
    if (partition == nullptr) {
        std::cerr << "PFS partition not found: " << options.partition << '\n';
        return false;
    }

    volume = std::make_unique<ps2hdd::ApaVolume>(device, *partition);
    fs = std::make_unique<ps2hdd::pfs::FileSystem>(*volume);
    if (!fs->mount()) {
        for (const auto& error : fs->probe_result().errors) std::cerr << "PFS mount error: " << error << '\n';
        return false;
    }
    return true;
}

bool extract_file(ps2hdd::pfs::FileSystem& fs, std::string_view path, const std::string& host_path)
{
    std::string error;
    const auto node = fs.resolve(path, &error);
    if (!node) {
        std::cerr << "PFS resolve error: " << error << '\n';
        return false;
    }
    if (node->is_directory()) {
        std::cerr << "PFS path is a directory, not a file.\n";
        return false;
    }

    std::ofstream output(host_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Could not create host file: " << host_path << '\n';
        return false;
    }

    constexpr std::size_t chunk_size = 1024 * 1024;
    std::vector<std::byte> buffer(chunk_size);
    std::uint64_t offset = 0;
    while (offset < node->inode.size) {
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), node->inode.size - offset));
        std::size_t bytes_read = 0;
        if (!fs.read_file(*node, offset, std::span<std::byte>(buffer).first(wanted), bytes_read, &error)) {
            std::cerr << "PFS read error: " << error << '\n';
            return false;
        }
        if (bytes_read == 0) {
            std::cerr << "PFS read returned EOF before the inode size.\n";
            return false;
        }
        output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(bytes_read));
        if (!output) {
            std::cerr << "Host write failed: " << host_path << '\n';
            return false;
        }
        offset += bytes_read;
    }

    std::cout << "Extracted " << path << " -> " << host_path << " (" << format_bytes(node->inode.size) << ")\n";
    return true;
}

void print_scan(ps2hdd::BlockDevice& device, const ps2hdd::apa::ScanResult& result)
{
    std::cout << "Source: " << device.display_name() << '\n'
              << "Size:   " << format_bytes(device.size_bytes()) << '\n'
              << "APA:    " << (result.mbr_valid ? "detected" : "not detected") << '\n';
    if (!result.mbr_valid) return;

    std::cout << "Version: " << result.apa_version << "\n\n";
    std::cout << std::left << std::setw(34) << "Partition"
              << std::setw(12) << "Type" << std::setw(16) << "Start LBA"
              << std::setw(16) << "Main size" << std::setw(12) << "Subs" << "Total\n";
    std::cout << std::string(102, '-') << '\n';

    for (const auto& p : result.partitions) {
        std::cout << std::left << std::setw(34) << (p.id.empty() ? "<unnamed>" : p.id)
                  << std::setw(12) << ps2hdd::apa::type_name(p.type)
                  << std::setw(16) << p.start_lba
                  << std::setw(16) << format_bytes(static_cast<std::uint64_t>(p.length_sectors) * ps2hdd::apa::kSectorSize)
                  << std::setw(12) << p.sub_count << format_bytes(p.size_bytes())
                  << (p.is_sub() ? "  [SUB]" : "") << '\n';

        if (p.type == ps2hdd::apa::kTypePfs && !p.is_sub()) {
            ps2hdd::ApaVolume volume(device, p);
            const auto pfs = ps2hdd::pfs::probe(volume);
            if (pfs.valid) {
                std::cout << "    PFS v" << pfs.super.version
                          << ", zone " << format_bytes(pfs.super.zone_size)
                          << ", filesystem subs " << pfs.super.num_subs
                          << ", backup " << (pfs.backup_matches ? "matches" : "differs/unreadable") << '\n';
                for (const auto& warning : pfs.warnings) std::cout << "      [PFS WARN] " << warning << '\n';
            } else {
                for (const auto& error : pfs.errors) std::cout << "      [PFS ERROR] " << error << '\n';
            }
        }
    }
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

    Options options;
    if (!parse_action(argc, argv, next_arg, options)) {
        usage();
        return 2;
    }

    ps2hdd::apa::Reader reader(*device);
    const auto scan = reader.scan();
    if (options.action == Action::scan) {
        print_scan(*device, scan);
    } else {
        if (!scan.ok()) {
            std::cerr << "APA scan failed; refusing to browse PFS.\n";
            return 1;
        }

        std::unique_ptr<ps2hdd::ApaVolume> volume;
        std::unique_ptr<ps2hdd::pfs::FileSystem> fs;
        if (!mount_selected_pfs(*device, scan, options, volume, fs)) return 1;

        if (options.action == Action::extract) {
            if (!extract_file(*fs, options.pfs_path, options.host_path)) return 1;
        } else {
            std::string error;
            const auto node = fs->resolve(options.pfs_path, &error);
            if (!node) {
                std::cerr << "PFS resolve error: " << error << '\n';
                return 1;
            }
            if (!node->is_directory()) {
                std::cerr << "PFS path is not a directory.\n";
                return 1;
            }

            std::cout << "Source:    " << device->display_name() << '\n'
                      << "Partition: " << options.partition << '\n'
                      << "Path:      " << options.pfs_path << "\n\n";

            if (options.action == Action::browse) {
                if (!print_directory(*fs, node->location)) return 1;
            } else {
                std::set<std::uint64_t> visited;
                if (!print_tree(*fs, node->location, "", 0, visited)) return 1;
            }
        }
    }

    if (!scan.issues.empty()) {
        std::cout << "\nDiagnostics:\n";
        for (const auto& issue : scan.issues) {
            std::cout << "  [" << (issue.severity == ps2hdd::apa::IssueSeverity::error ? "ERROR" : "WARN")
                      << "] LBA " << issue.lba << ": " << issue.message << '\n';
        }
    }

    return scan.ok() ? 0 : 1;
}
