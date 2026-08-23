#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_remove.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/pfs_write.hpp"
#include "ps2hdd/version.hpp"
#include "ps2hdd/writable_file_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
        << "  ps2-driveforge-pfs-tools put <disk-image> <pfs-partition-id>\n"
        << "      <host-file> <pfs-path> --apply\n"
        << "  ps2-driveforge-pfs-tools remove-partition <disk-image>\n"
        << "      <partition-id|lba> --apply\n\n"
        << "The writer creates/replaces regular files inside existing PFS directories.\n"
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
              << "  partition:       " << plan.partition_id << '\n'
              << "  main LBA:        " << plan.main_lba << '\n'
              << "  detached headers:" << ' ' << plan.removed_lbas.size() << '\n'
              << "  rewritten links: " << plan.link_rewrites.size() << '\n'
              << "  freed bytes:      " << plan.freed_bytes << "\n\n";
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

    const auto result = ps2hdd::apa::remove_main_partition_from_image(image, partition->start_lba);
    if (!result.ok) {
        std::cerr << "Fast APA removal failed: " << result.error << '\n';
        return 1;
    }
    std::cout << "APA partition removed and chain reparsed successfully\n"
              << "  partition:        " << result.partition_id << '\n'
              << "  detached headers: " << result.removed_headers << '\n'
              << "  rewritten headers:" << ' ' << result.rewritten_headers << '\n'
              << "  freed bytes:       " << result.freed_bytes << '\n';
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
    const auto result = writer.write_file(argv[5], *bytes);
    if (!result.ok) {
        std::cerr << "PFS file mutation failed: " << result.error << '\n';
        if (!result.warning.empty()) {
            std::cerr << "WARNING: " << result.warning << '\n';
        }
        return 1;
    }

    std::cout << (result.disposition == ps2hdd::pfs::FileWriteDisposition::created
                      ? "PFS file created"
                      : "PFS file replaced copy-on-write")
              << " and verified through the normal reader\n"
              << "  path:                  " << result.path << '\n'
              << "  bytes:                 " << result.bytes << '\n'
              << "  payload write calls:   " << result.payload_write_calls << '\n'
              << "  metadata transactions: " << result.metadata_transactions << '\n'
              << "  bitmap chunks touched: " << result.bitmap_chunks_touched << '\n';
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
    if (command == "put") {
        return put_file(argc, argv);
    }
    usage();
    return 2;
}
