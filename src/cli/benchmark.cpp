#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/version.hpp"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
#include "ps2hdd/physical_drive.hpp"
#endif

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;

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

std::string join_path(std::string_view parent, std::string_view child)
{
    if (parent.empty() || parent == "/") {
        return std::string(child);
    }
    std::string result(parent);
    if (result.back() != '/') {
        result.push_back('/');
    }
    result.append(child);
    return result;
}

std::uint64_t elapsed_ns(Clock::time_point started)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

void print_io(std::string_view label, const ps2hdd::SessionStats& stats, std::uint64_t wall_ns)
{
    const auto& io = stats.backing_io;
    const auto& raw = stats.read_cache;
    const auto& ahead = stats.read_ahead;
    const auto& cache = stats.cache;
    const double wall_ms = static_cast<double>(wall_ns) / 1'000'000.0;
    const double io_ms = static_cast<double>(io.read_time_ns) / 1'000'000.0;

    std::cout << "\n[" << label << "]\n"
              << "  wall time:              " << std::fixed << std::setprecision(3) << wall_ms << " ms\n"
              << "  backing read calls:     " << io.read_calls << '\n'
              << "  backing bytes:          " << format_bytes(io.bytes_requested) << '\n'
              << "  backing service time:   " << std::fixed << std::setprecision(3) << io_ms << " ms\n"
              << "  small backing reads:    " << io.small_read_calls << '\n'
              << "  largest backing read:   " << format_bytes(io.largest_read) << '\n'
              << "  max reads in flight:    " << io.max_in_flight << '\n'
              << "  read-window hit/miss:   " << raw.hits << '/' << raw.misses << '\n'
              << "  read-window fill bytes: " << format_bytes(raw.fill_bytes) << '\n'
              << "  read-ahead hit/miss:    " << ahead.window_hits << '/' << ahead.window_misses << '\n'
              << "  prefetch count/bytes:   " << ahead.prefetches << '/' << format_bytes(ahead.prefetched_bytes) << '\n'
              << "  probe hit/miss:         " << cache.probe_hits << '/' << cache.probe_misses << '\n'
              << "  browse hit/miss:        " << cache.browse_hits << '/' << cache.browse_misses << '\n'
              << "  stat hit/miss:          " << cache.stat_hits << '/' << cache.stat_misses << '\n'
              << "  node hit/miss:          " << cache.node_hits << '/' << cache.node_misses << '\n';
}

void usage()
{
    std::cout << "PS2 DriveForge " << ps2hdd::version::string << "-dev ("
              << ps2hdd::version::codename << ") performance harness\n\n"
              << "Usage:\n"
              << "  ps2-driveforge-benchmark <disk-image> [--browse <partition> [path]]\n"
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
              << "  ps2-driveforge-benchmark --physical <index> [--browse <partition> [path]]\n"
#endif
              << "\nThe harness is read-only. Cold and warm metadata workloads are reported separately.\n";
}

struct BrowseTarget {
    std::string partition;
    std::string path;
};

bool metadata_workload(ps2hdd::DriveSession& session, const BrowseTarget& target)
{
    const auto browse = session.browse(target.partition, target.path);
    if (!browse.ok) {
        std::cerr << "Browse failed: " << browse.error << '\n';
        return false;
    }
    for (const auto& entry : browse.entries) {
        const auto stat = session.stat(target.partition, join_path(target.path, entry.name));
        if (!stat.ok) {
            std::cerr << "Stat failed for " << entry.name << ": " << stat.error << '\n';
            return false;
        }
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
    std::unique_ptr<ps2hdd::BlockDevice> source;
#ifdef PS2DF_HAS_WINDOWS_PHYSICAL_DRIVE
    if (std::string_view(argv[arg]) == "--physical") {
        if (arg + 1 >= argc) {
            usage();
            return 2;
        }
        try {
            auto physical = std::make_unique<ps2hdd::PhysicalDrive>(
                static_cast<unsigned>(std::stoul(argv[arg + 1])));
            if (!physical->is_open()) {
                std::cerr << "Could not open PhysicalDrive read-only. Win32 error: "
                          << physical->open_error() << '\n';
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
        auto image = std::make_unique<ps2hdd::FileBlockDevice>(argv[arg]);
        if (!image->is_open()) {
            std::cerr << "Could not open image: " << argv[arg] << '\n';
            return 1;
        }
        source = std::move(image);
        ++arg;
    }

    std::optional<BrowseTarget> target;
    if (arg < argc) {
        if (std::string_view(argv[arg]) != "--browse" || arg + 1 >= argc || arg + 3 < argc) {
            usage();
            return 2;
        }
        BrowseTarget request;
        request.partition = argv[arg + 1];
        if (arg + 2 < argc) {
            request.path = argv[arg + 2];
        }
        target = std::move(request);
    }

    ps2hdd::DriveSession session(std::move(source));
    session.reset_stats();
    const auto scan_started = Clock::now();
    if (!session.scan()) {
        std::cerr << "APA scan failed: " << session.last_error() << '\n';
        return 1;
    }
    const auto scan_wall = elapsed_ns(scan_started);
    print_io("cold APA scan", session.stats(), scan_wall);

    const auto catalog_started = Clock::now();
    const auto catalog = session.partition_catalog(true);
    const auto catalog_wall = elapsed_ns(catalog_started);
    std::cout << "\n[zero-I/O partition catalog]\n"
              << "  build time:             " << std::fixed << std::setprecision(3)
              << static_cast<double>(catalog_wall) / 1'000'000.0 << " ms\n"
              << "  rows:                   " << catalog.entries.size() << '\n'
              << "  main/sub:               " << catalog.main_partitions << '/' << catalog.sub_partitions << '\n'
              << "  HDL partitions:         " << catalog.hdl_partitions << '\n'
              << "  PFS partitions:         " << catalog.pfs_partitions << '\n'
              << "  HDL bytes:              " << format_bytes(catalog.hdl_bytes) << '\n'
              << "  PFS bytes:              " << format_bytes(catalog.pfs_bytes) << '\n'
              << "  free bytes:             " << format_bytes(catalog.free_bytes) << '\n';

    if (!target) {
        return 0;
    }

    session.clear_caches();
    session.reset_stats();
    const auto cold_started = Clock::now();
    if (!metadata_workload(session, *target)) {
        return 1;
    }
    print_io("cold browse + stat", session.stats(), elapsed_ns(cold_started));

    session.reset_stats();
    const auto warm_started = Clock::now();
    if (!metadata_workload(session, *target)) {
        return 1;
    }
    print_io("warm browse + stat", session.stats(), elapsed_ns(warm_started));
    return 0;
}
