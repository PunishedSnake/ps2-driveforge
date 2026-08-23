#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_remove.hpp"
#include "ps2hdd/disk_layout_guard.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/pfs_write.hpp"
#include "ps2hdd/version.hpp"
#include "ps2hdd/writable_file_block_device.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage()
{
    std::cout
        << "PS2 DriveForge " << ps2hdd::version::string << "-dev ("
        << ps2hdd::version::codename << ") - Frieren PFS/APA developer tools\n\n"
        << "Read-only commands:\n"
        << "  ps2-driveforge-pfs-tools plan-remove <disk-image> <partition-id|lba>\n\n"
        << "Image-only mutation commands (explicit --apply required):\n"
        << "  ps2-driveforge-pfs-tools mkdir <disk-image> <pfs-partition-id>\n"
        << "      <pfs-directory> --apply\n"
        << "  ps2-driveforge-pfs-tools put <disk-image> <pfs-partition-id>\n"
        << "      <host-file> <pfs-path> --apply\n"
        << "  ps2-driveforge-pfs-tools remove-partition <disk-image>\n"
        << "      <partition-id|lba> --apply\n\n"
        << "put creates missing parent directories through the same bounded PFS writer.\n"
        << "Protective/hybrid GPT layouts are refused before image mutation.\n"
        << "PhysicalDrive write access is intentionally unavailable in Frieren here.\n";
}

std::optional<std::uint32_t> parse_u32(std::string_view text)
{
    try {
        std::size_t used = 0;
        const auto value = std::stoull(std::string(text), &used, 0);
        if (used != text.size() || value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(value);
    } catch (...) {
        return std::nullopt;
    }
}

const ps2hdd::apa::Partition* find_main_partition(const ps2hdd::apa::ScanResult& scan,
                                                   std::string_view target,
                                                   std::optional<std::uint16_t> type = std::nullopt)
{
    const auto lba = parse_u32(target);
    const ps2hdd::apa::Partition* found = nullptr;
    for (const auto& partition : scan.partitions) {
        if (partition.is_sub()) {
            continue;
        }
        if (type && partition.type != *type) {
            continue;
        }
        const bool matches = lba ? partition.start_lba == *lba : partition.id == target;
        if (!matches) {
            continue;
        }
        if (found != nullptr) {
            return nullptr;
        }
        found = &partition;
    }
    return found;
}

bool mutation_layout_allowed(ps2hdd::BlockDevice& image)
{
    const auto guard = ps2hdd::inspect_disk_layout(image);
    if (!guard.ok) {
        std::cerr << "Could not validate PC partition-map safety before mutation: "
                  << guard.error << '\n';
        return false;
    }
    if (!guard.allows_ps2_mutation()) {
        std::cerr << "Refusing PS2 image mutation because the target contains "
                  << ps2hdd::legacy_partition_map_name(guard.kind) << " evidence.\n";
        return false;
    }
    return true;
}

std::optional<std::vector<std::byte>> read_host_file(const std::filesystem::path& path)
{
    constexpr std::uint64_t kMaxPfsToolFileBytes = 64ULL * 1024ULL * 1024ULL;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxPfsToolFileBytes) {
        std::cerr << "Host file is missing or exceeds the current 64 MiB PFS tool limit: "
                  << path.string() << '\n';
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Could not open host file: " << path.string() << '\n';
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            std::cerr << "Could not read complete host file: " << path.string() << '\n';
            return std::nullopt;
        }
    }
    return bytes;
}

std::string parent_path(std::string_view path)
{
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos) {
        return {};
    }
    if (slash == 0) {
        return "/";
    }
    return std::string(path.substr(0, slash));
}

int plan_remove(int argc, char** argv)
{
    if (argc != 4) {
        usage();
        return 2;
    }
    ps2hdd::FileBlockDevice image(argv[2]);
    if (!image.is_open()) {
        std::cerr << "Could not open disk image read-only: " << argv[2] << '\n';
        return 1;
    }
    ps2hdd::apa::Reader reader(image);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        std::cerr << "Refusing removal plan because APA scan is not clean.\n";
        return 1;
    }
    const auto* partition = find_main_partition(scan, argv[3]);
    if (partition == nullptr || partition->start_lba == 0) {
        std::cerr << "Unique removable main partition not found: " << argv[3] << '\n';
        return 1;
    }

    const auto plan = ps2hdd::apa::plan_remove_main_partition(scan, partition->start_lba);
    if (!plan.ok) {
        std::cerr << "APA removal plan failed: " << plan.error << '\n';
        return 1;
    }

    std::cout << "Fast APA removal plan\n"
              << "  partition:        " << plan.partition_id << '\n'
              << "  main LBA:         " << plan.main_lba << '\n'
              << "  detached headers: " << plan.removed_lbas.size() << '\n'
              << "  rewritten links:  " << plan.link_rewrites.size() << '\n'
              << "  freed bytes:       " << plan.freed_bytes << "\n\n";
    for (const auto& rewrite : plan.link_rewrites) {
        std::cout << "  LBA " << rewrite.header_lba
                  << " prev " << rewrite.old_prev_lba << " -> " << rewrite.new_prev_lba
                  << ", next " << rewrite.old_next_lba << " -> " << rewrite.new_next_lba << '\n';
    }
    std::cout << "\nDry-run only: no partition payload or headers were zero-filled.\n";
    return 0;
}

int remove_partition(int argc, char** argv)
{
    if (argc != 5 || std::string_view(argv[4]) != "--apply") {
        std::cerr << "remove-partition requires an explicit trailing --apply.\n";
        return 2;
    }
    ps2hdd::WritableFileBlockDevice image(argv[2]);
    if (!image.is_open()) {
        std::cerr << "Could not open existing disk image for read/write access: " << argv[2] << '\n';
        return 1;
    }
    if (!mutation_layout_allowed(image)) {
        return 1;
    }
    ps2hdd::apa::Reader reader(image);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        std::cerr << "Refusing partition removal because APA scan is not clean.\n";
        return 1;
    }
    const auto* partition = find_main_partition(scan, argv[3]);
    if (partition == nullptr || partition->start_lba == 0) {
        std::cerr << "Unique removable main partition not found: " << argv[3] << '\n';
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto result = ps2hdd::apa::remove_main_partition_from_image(image, partition->start_lba);
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (!result.ok) {
        std::cerr << "Fast APA removal failed: " << result.error << '\n';
        return 1;
    }
    std::cout << "APA partition removed and chain reparsed successfully\n"
              << "  partition:         " << result.partition_id << '\n'
              << "  detached headers:  " << result.removed_headers << '\n'
              << "  rewritten headers: " << result.rewritten_headers << '\n'
              << "  freed bytes:        " << result.freed_bytes << '\n'
              << "  mutation + verify:  " << std::fixed << std::setprecision(3)
              << elapsed << " ms\n";
    return 0;
}

int mkdir_path(int argc, char** argv)
{
    if (argc != 6 || std::string_view(argv[5]) != "--apply") {
        std::cerr << "mkdir requires: <image> <PFS partition> <PFS directory> --apply\n";
        return 2;
    }

    ps2hdd::WritableFileBlockDevice image(argv[2]);
    if (!image.is_open()) {
        std::cerr << "Could not open existing disk image for read/write access: " << argv[2] << '\n';
        return 1;
    }
    if (!mutation_layout_allowed(image)) {
        return 1;
    }
    ps2hdd::apa::Reader reader(image);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        std::cerr << "Refusing PFS mkdir because APA scan is not clean.\n";
        return 1;
    }
    const auto* partition = find_main_partition(scan, argv[3], ps2hdd::apa::kTypePfs);
    if (partition == nullptr) {
        std::cerr << "Unique PFS main partition not found: " << argv[3] << '\n';
        return 1;
    }

    ps2hdd::pfs::ImageWriter writer(image, *partition);
    if (!writer.valid()) {
        std::cerr << "PFS writer refused partition: " << writer.error() << '\n';
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto result = writer.ensure_directory(argv[4]);
    const auto elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (!result.ok) {
        std::cerr << "PFS mkdir failed: " << result.error << '\n';
        if (!result.warning.empty()) {
            std::cerr << "WARNING: " << result.warning << '\n';
        }
        return 1;
    }

    std::cout << "PFS directory tree verified\n"
              << "  path:                  " << result.path << '\n'
              << "  components created:    " << result.created_components << '\n'
              << "  metadata transactions: " << result.metadata_transactions << '\n'
              << "  bitmap chunks touched: " << result.bitmap_chunks_touched << '\n'
              << "  mutation + verify:      " << std::fixed << std::setprecision(3)
              << elapsed_ms << " ms\n";
    if (!result.warning.empty()) {
        std::cerr << "WARNING: " << result.warning << '\n';
    }
    return 0;
}

int put_file(int argc, char** argv)
{
    if (argc != 7 || std::string_view(argv[6]) != "--apply") {
        std::cerr << "put requires: <image> <PFS partition> <host file> <PFS path> --apply\n";
        return 2;
    }
    const auto bytes = read_host_file(argv[4]);
    if (!bytes) {
        return 1;
    }

    ps2hdd::WritableFileBlockDevice image(argv[2]);
    if (!image.is_open()) {
        std::cerr << "Could not open existing disk image for read/write access: " << argv[2] << '\n';
        return 1;
    }
    if (!mutation_layout_allowed(image)) {
        return 1;
    }
    ps2hdd::apa::Reader reader(image);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        std::cerr << "Refusing PFS mutation because APA scan is not clean.\n";
        return 1;
    }
    const auto* partition = find_main_partition(scan, argv[3], ps2hdd::apa::kTypePfs);
    if (partition == nullptr) {
        std::cerr << "Unique PFS main partition not found: " << argv[3] << '\n';
        return 1;
    }

    ps2hdd::pfs::ImageWriter writer(image, *partition);
    if (!writer.valid()) {
        std::cerr << "PFS writer refused partition: " << writer.error() << '\n';
        return 1;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto parent = parent_path(argv[5]);
    ps2hdd::pfs::DirectoryEnsureResult parents;
    if (!parent.empty() && parent != "/") {
        parents = writer.ensure_directory(parent);
        if (!parents.ok) {
            std::cerr << "Could not prepare PFS parent directories: " << parents.error << '\n';
            return 1;
        }
    }

    const auto result = writer.write_file(argv[5], *bytes);
    const double elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    if (!result.ok) {
        std::cerr << "PFS file mutation failed: " << result.error << '\n';
        if (!result.warning.empty()) {
            std::cerr << "WARNING: " << result.warning << '\n';
        }
        return 1;
    }

    const double mib = static_cast<double>(result.bytes) / (1024.0 * 1024.0);
    const double mib_per_second = elapsed_seconds > 0.0 ? mib / elapsed_seconds : 0.0;
    std::cout << (result.disposition == ps2hdd::pfs::FileWriteDisposition::created
                      ? "PFS file created"
                      : "PFS file replaced copy-on-write")
              << " and verified through the normal reader\n"
              << "  path:                  " << result.path << '\n'
              << "  parent dirs created:   " << parents.created_components << '\n'
              << "  bytes:                 " << result.bytes << '\n'
              << "  payload write calls:   " << result.payload_write_calls << '\n'
              << "  metadata transactions: "
              << result.metadata_transactions + parents.metadata_transactions << '\n'
              << "  bitmap chunks touched: "
              << result.bitmap_chunks_touched + parents.bitmap_chunks_touched << '\n'
              << "  mutation + verify:      " << std::fixed << std::setprecision(3)
              << elapsed_seconds * 1000.0 << " ms\n"
              << "  effective payload rate: " << std::setprecision(2)
              << mib_per_second << " MiB/s\n";
    if (!parents.warning.empty()) {
        std::cerr << "WARNING: " << parents.warning << '\n';
    }
    if (!result.warning.empty()) {
        std::cerr << "WARNING: " << result.warning << '\n';
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string_view command = argv[1];
    if (command == "--version") {
        std::cout << "PS2 DriveForge " << ps2hdd::version::string << "-dev ("
                  << ps2hdd::version::codename << ")\n";
        return 0;
    }
    if (command == "plan-remove") {
        return plan_remove(argc, argv);
    }
    if (command == "remove-partition") {
        return remove_partition(argc, argv);
    }
    if (command == "mkdir") {
        return mkdir_path(argc, argv);
    }
    if (command == "put") {
        return put_file(argc, argv);
    }
    usage();
    return 2;
}
