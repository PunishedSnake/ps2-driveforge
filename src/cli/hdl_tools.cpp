#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_allocation.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/hdl.hpp"
#include "ps2hdd/hdl_image_install.hpp"
#include "ps2hdd/hdl_install_plan.hpp"
#include "ps2hdd/http_client.hpp"
#include "ps2hdd/opl_asset_pipeline.hpp"
#include "ps2hdd/opl_assets.hpp"
#include "ps2hdd/ps2_iso.hpp"
#include "ps2hdd/version.hpp"
#include "ps2hdd/writable_file_block_device.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

void usage()
{
    std::cout
        << "PS2 DriveForge " << ps2hdd::version::string << "-dev ("
        << ps2hdd::version::codename << ") - Frieren HDL developer tools\n\n"
        << "Read-only / host-staging commands:\n"
        << "  ps2-driveforge-hdl-tools inspect-iso <game.iso>\n"
        << "  ps2-driveforge-hdl-tools plan <disk-image> <payload-mib>\n"
        << "  ps2-driveforge-hdl-tools plan-install <disk-image> <game.iso>\n"
        << "      [--title <text>] [--hidden]\n"
        << "  ps2-driveforge-hdl-tools plan-assets <game.iso>\n"
        << "      [--title <text>] [--tar] [--hdd-osd] [--all-art]\n"
        << "  ps2-driveforge-hdl-tools fetch-assets <game.iso> <cache-dir>\n"
        << "      [--title <text>] [--tar] [--hdd-osd] [--all-art] [--refresh-catalog]\n\n"
        << "Image-only mutation commands (explicit --apply required):\n"
        << "  ps2-driveforge-hdl-tools install-image <disk-image> <game.iso>\n"
        << "      [--title <text>] [--hidden] [--cd|--dvd] [--compat <0..255>]\n"
        << "      [--dma <0..65535>] [--layer-break <sectors>] --apply\n"
        << "  ps2-driveforge-hdl-tools patch-metadata <disk-image> <partition-id>\n"
        << "      [--title <text>] [--compat <0..255>] [--dma <0..65535>] --apply\n\n"
        << "Asset downloads are staged on the host only. PFS import is intentionally a\n"
        << "separate Frieren write milestone. PhysicalDrive writes do not exist here.\n";
}

std::optional<std::uint64_t> parse_u64(std::string_view text)
{
    try {
        std::size_t used = 0;
        const auto value = std::stoull(std::string(text), &used, 0);
        if (used != text.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

const ps2hdd::apa::Partition* find_hdl_partition(const ps2hdd::apa::ScanResult& scan,
                                                  std::string_view id)
{
    for (const auto& partition : scan.partitions) {
        if (!partition.is_sub() && partition.type == ps2hdd::apa::kTypeHdl && partition.id == id) {
            return &partition;
        }
    }
    return nullptr;
}

void print_allocation(const ps2hdd::apa::AllocationPlan& result)
{
    std::cout << "  allocation chunks: " << result.free_chunks_before - result.free_chunks_after
              << " x 128 MiB\n"
              << "  APA extents:       " << result.extents.size() << " (1 main + "
              << result.sub_count() << " sub)\n"
              << "  allocated:         " << result.allocated_bytes / (1024ULL * 1024ULL) << " MiB\n"
              << "  HDL overhead:      " << result.overhead_bytes / (1024ULL * 1024ULL) << " MiB\n"
              << "  usable payload:    " << result.usable_payload_bytes / (1024ULL * 1024ULL) << " MiB\n"
              << "  free chunks:       " << result.free_chunks_before << " -> "
              << result.free_chunks_after << "\n\n";

    std::cout << "Planned extents:\n";
    for (std::size_t i = 0; i < result.extents.size(); ++i) {
        const auto& extent = result.extents[i];
        std::cout << "  " << (i == 0 ? "MAIN" : "SUB ") << ' ' << std::setw(2) << i
                  << " start LBA " << std::setw(10) << extent.start_lba
                  << " sectors " << std::setw(10) << extent.length_sectors
                  << " prev " << std::setw(10) << extent.prev_lba
                  << " next " << extent.next_lba << '\n';
    }

    std::cout << "\nExisting APA headers whose links would change: "
              << result.existing_link_updates.size() << '\n';
    for (const auto& update : result.existing_link_updates) {
        std::cout << "  LBA " << update.header_lba
                  << " prev " << update.old_prev_lba << " -> " << update.new_prev_lba
                  << ", next " << update.old_next_lba << " -> " << update.new_next_lba << '\n';
    }
}

int inspect_iso(const char* image_path)
{
    ps2hdd::FileBlockDevice image(image_path);
    if (!image.is_open()) {
        std::cerr << "Could not open ISO: " << image_path << '\n';
        return 1;
    }
    const auto result = ps2hdd::iso::inspect_ps2_iso(image);
    if (!result.ok) {
        std::cerr << "PS2 ISO inspection failed: " << result.error << '\n';
        return 1;
    }

    std::cout << "PS2 ISO preflight\n"
              << "  source:  " << image.display_name() << '\n'
              << "  bytes:   " << result.game.image_bytes << '\n'
              << "  startup: " << result.game.startup << '\n'
              << "\nSYSTEM.CNF:\n" << result.game.system_cnf << '\n';
    return 0;
}

int plan(const char* image_path, std::string_view payload_mib_text)
{
    const auto payload_mib = parse_u64(payload_mib_text);
    if (!payload_mib || *payload_mib == 0 ||
        *payload_mib > std::numeric_limits<std::uint64_t>::max() / (1024ULL * 1024ULL)) {
        std::cerr << "Invalid payload size in MiB.\n";
        return 2;
    }

    ps2hdd::FileBlockDevice image(image_path);
    if (!image.is_open()) {
        std::cerr << "Could not open image read-only: " << image_path << '\n';
        return 1;
    }

    ps2hdd::apa::Reader reader(image);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        std::cerr << "Refusing allocation plan because the APA scan is not clean.\n";
        for (const auto& issue : scan.issues) {
            std::cerr << "  LBA " << issue.lba << ": " << issue.message << '\n';
        }
        return 1;
    }

    const auto payload_bytes = *payload_mib * 1024ULL * 1024ULL;
    const auto result = ps2hdd::apa::plan_hdl_allocation(scan, image.size_bytes(), payload_bytes);
    if (!result.ok) {
        std::cerr << "HDL allocation plan failed: " << result.error << '\n';
        return 1;
    }

    std::cout << "HDL allocation dry-run\n"
              << "  source:            " << image.display_name() << '\n'
              << "  payload:           " << *payload_mib << " MiB\n";
    print_allocation(result);
    std::cout << "\nDry-run only: zero source writes performed.\n";
    return 0;
}

int plan_install(int argc, char** argv)
{
    if (argc < 4) {
        usage();
        return 2;
    }
    const char* disk_path = argv[2];
    const char* iso_path = argv[3];
    std::string title = std::filesystem::path(iso_path).stem().string();
    bool hidden = false;

    for (int i = 4; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--title") {
            if (++i >= argc) {
                std::cerr << "--title requires a value.\n";
                return 2;
            }
            title = argv[i];
        } else if (option == "--hidden") {
            hidden = true;
        } else {
            std::cerr << "Unknown install-plan option: " << option << '\n';
            return 2;
        }
    }

    ps2hdd::FileBlockDevice disk(disk_path);
    if (!disk.is_open()) {
        std::cerr << "Could not open PS2 disk image read-only: " << disk_path << '\n';
        return 1;
    }
    ps2hdd::FileBlockDevice game(iso_path);
    if (!game.is_open()) {
        std::cerr << "Could not open game ISO read-only: " << iso_path << '\n';
        return 1;
    }

    ps2hdd::apa::Reader reader(disk);
    const auto scan = reader.scan();
    const auto result = ps2hdd::hdl::plan_install(scan, disk.size_bytes(), game, title, hidden);
    if (!result.ok) {
        std::cerr << "HDL install preflight failed: " << result.error << '\n';
        return 1;
    }

    std::cout << "HDL install dry-run\n"
              << "  disk image:        " << disk.display_name() << '\n'
              << "  game ISO:          " << game.display_name() << '\n'
              << "  ISO bytes:         " << result.source.image_bytes << '\n'
              << "  startup:           " << result.source.startup << '\n'
              << "  title:             " << result.title << '\n'
              << "  APA partition ID:  " << result.partition_id << '\n';
    print_allocation(result.allocation);
    std::cout << "\nInstall dry-run complete: zero source writes performed.\n";
    return 0;
}

struct ParsedAssetOptions {
    ps2hdd::opl::AssetOptions assets;
    bool refresh_catalog{};
    std::string title;
};

std::optional<ParsedAssetOptions> parse_asset_options(int argc, char** argv, int first,
                                                       std::string default_title)
{
    ParsedAssetOptions parsed;
    parsed.title = std::move(default_title);
    for (int i = first; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--title") {
            if (++i >= argc) {
                std::cerr << "--title requires a value.\n";
                return std::nullopt;
            }
            parsed.title = argv[i];
        } else if (option == "--tar") {
            parsed.assets.layout = ps2hdd::opl::LayoutMode::tar_archives;
        } else if (option == "--hdd-osd") {
            parsed.assets.hdd_osd_icon = true;
        } else if (option == "--all-art") {
            parsed.assets.artwork_logo = true;
            parsed.assets.artwork_label = true;
        } else if (option == "--refresh-catalog") {
            parsed.refresh_catalog = true;
        } else {
            std::cerr << "Unknown asset option: " << option << '\n';
            return std::nullopt;
        }
    }
    return parsed;
}

std::optional<ps2hdd::opl::AssetPlan> make_iso_asset_plan(const char* iso_path,
                                                           const ParsedAssetOptions& parsed)
{
    ps2hdd::FileBlockDevice game(iso_path);
    if (!game.is_open()) {
        std::cerr << "Could not open game ISO read-only: " << iso_path << '\n';
        return std::nullopt;
    }
    const auto inspected = ps2hdd::iso::inspect_ps2_iso(game);
    if (!inspected.ok) {
        std::cerr << "PS2 ISO inspection failed before asset lookup: " << inspected.error << '\n';
        return std::nullopt;
    }

    auto plan = ps2hdd::opl::plan_assets({inspected.game.startup, parsed.title}, parsed.assets);
    if (!plan.ok) {
        std::cerr << "OPL asset planning failed: " << plan.error << '\n';
        return std::nullopt;
    }
    return plan;
}

void print_asset_plan(const ps2hdd::opl::AssetPlan& plan)
{
    std::cout << "OPL asset plan\n"
              << "  game ID:  " << plan.game_id << '\n'
              << "  title:    " << plan.title << '\n'
              << "  requests: " << plan.requests.size() << "\n\n";

    for (const auto& request : plan.requests) {
        std::cout << ps2hdd::opl::asset_kind_name(request.kind)
                  << (request.optional ? " [optional]" : " [required]") << '\n';
        for (const auto& candidate : request.candidates) {
            std::cout << "  provider: " << candidate.provider << '\n'
                      << "  source:   " << candidate.url << '\n'
                      << "  target:   " << candidate.target_path;
            if (!candidate.archive_member.empty()) {
                std::cout << " :: " << candidate.archive_member;
            }
            std::cout << '\n';
        }
        std::cout << '\n';
    }
}

int plan_assets(int argc, char** argv)
{
    if (argc < 3) {
        usage();
        return 2;
    }
    const char* iso_path = argv[2];
    const auto parsed = parse_asset_options(argc, argv, 3,
                                             std::filesystem::path(iso_path).stem().string());
    if (!parsed) {
        return 2;
    }
    const auto plan = make_iso_asset_plan(iso_path, *parsed);
    if (!plan) {
        return 1;
    }
    print_asset_plan(*plan);
    std::cout << "Host/PFS writes performed: zero.\n";
    return 0;
}

int fetch_assets(int argc, char** argv)
{
    if (argc < 4) {
        usage();
        return 2;
    }
    const char* iso_path = argv[2];
    const std::filesystem::path cache_root = argv[3];
    const auto parsed = parse_asset_options(argc, argv, 4,
                                             std::filesystem::path(iso_path).stem().string());
    if (!parsed) {
        return 2;
    }
    const auto plan = make_iso_asset_plan(iso_path, *parsed);
    if (!plan) {
        return 1;
    }

    std::unique_ptr<ps2hdd::HttpClient> http = ps2hdd::make_platform_http_client();
    if (!http) {
        std::cerr << "Native HTTPS asset transport is not available on this platform build.\n";
        return 1;
    }

    ps2hdd::opl::FetchOptions fetch_options;
    fetch_options.refresh_catalog = parsed->refresh_catalog;
    const auto fetched = ps2hdd::opl::fetch_assets(*plan, *http, cache_root, fetch_options);

    std::cout << "OPL asset staging result\n"
              << "  game ID: " << plan->game_id << '\n'
              << "  staged:  " << fetched.assets.size() << '\n'
              << "  issues:  " << fetched.issues.size() << "\n\n";
    for (const auto& asset : fetched.assets) {
        std::cout << "  " << ps2hdd::opl::asset_kind_name(asset.kind) << '\n'
                  << "    provider: " << asset.provider << '\n'
                  << "    target:   " << asset.target_path;
        if (!asset.archive_member.empty()) {
            std::cout << " :: " << asset.archive_member;
        }
        std::cout << '\n'
                  << "    staged:   " << asset.staged_path.string() << '\n'
                  << "    bytes:    " << asset.bytes;
        if (asset.from_cache) {
            std::cout << " [cache]";
        }
        if (asset.merged) {
            std::cout << " [merged]";
        }
        if (asset.verified_mastercode) {
            std::cout << " [mastercode verified]";
        }
        std::cout << "\n\n";
    }
    for (const auto& issue : fetched.issues) {
        std::cerr << (issue.fatal ? "ERROR: " : "WARNING: ")
                  << ps2hdd::opl::asset_kind_name(issue.kind) << ": "
                  << issue.message << '\n';
    }
    std::cout << "PFS writes performed: zero. Assets are host-staged for the later PFS transaction.\n";
    return fetched.ok ? 0 : 1;
}

int install_image(int argc, char** argv)
{
    if (argc < 5) {
        usage();
        return 2;
    }
    const char* disk_path = argv[2];
    const char* iso_path = argv[3];
    ps2hdd::hdl::ImageInstallOptions options;
    options.title = std::filesystem::path(iso_path).stem().string();
    options.media = ps2hdd::hdl::MediaType::dvd;
    bool apply = false;

    for (int i = 4; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--apply") {
            apply = true;
        } else if (option == "--title") {
            if (++i >= argc) {
                std::cerr << "--title requires a value.\n";
                return 2;
            }
            options.title = argv[i];
        } else if (option == "--hidden") {
            options.hidden = true;
        } else if (option == "--cd") {
            options.media = ps2hdd::hdl::MediaType::cd;
        } else if (option == "--dvd") {
            options.media = ps2hdd::hdl::MediaType::dvd;
        } else if (option == "--compat") {
            if (++i >= argc) {
                std::cerr << "--compat requires a value.\n";
                return 2;
            }
            const auto value = parse_u64(argv[i]);
            if (!value || *value > 0xFFU) {
                std::cerr << "--compat must be in 0..255.\n";
                return 2;
            }
            options.compat_flags = static_cast<std::uint8_t>(*value);
        } else if (option == "--dma") {
            if (++i >= argc) {
                std::cerr << "--dma requires a value.\n";
                return 2;
            }
            const auto value = parse_u64(argv[i]);
            if (!value || *value > 0xFFFFU) {
                std::cerr << "--dma must be in 0..65535.\n";
                return 2;
            }
            options.dma = static_cast<std::uint16_t>(*value);
        } else if (option == "--layer-break") {
            if (++i >= argc) {
                std::cerr << "--layer-break requires a value.\n";
                return 2;
            }
            const auto value = parse_u64(argv[i]);
            if (!value || *value > std::numeric_limits<std::uint32_t>::max()) {
                std::cerr << "--layer-break must fit in 32 bits.\n";
                return 2;
            }
            options.layer_break = static_cast<std::uint32_t>(*value);
        } else {
            std::cerr << "Unknown install-image option: " << option << '\n';
            return 2;
        }
    }

    if (!apply) {
        std::cerr << "Refusing image mutation without explicit --apply. Use plan-install first.\n";
        return 2;
    }

    ps2hdd::WritableFileBlockDevice disk(disk_path);
    if (!disk.is_open()) {
        std::cerr << "Could not open existing PS2 disk image for explicit read/write access: "
                  << disk_path << '\n';
        return 1;
    }
    ps2hdd::FileBlockDevice game(iso_path);
    if (!game.is_open()) {
        std::cerr << "Could not open game ISO read-only: " << iso_path << '\n';
        return 1;
    }

    const auto result = ps2hdd::hdl::install_to_image(
        disk, game, options,
        [](const ps2hdd::hdl::ImageInstallProgress& progress) {
            std::cout << "[" << progress.phase << "] " << progress.copied_bytes
                      << '/' << progress.total_bytes << " bytes\n";
        });
    if (!result.ok) {
        std::cerr << "Image-only HDL install failed: " << result.error << '\n';
        if (result.orphan_payload_bytes_on_failure != 0) {
            std::cerr << "Unpublished payload bytes may remain in APA free space: "
                      << result.orphan_payload_bytes_on_failure << '\n';
        }
        return 1;
    }

    std::cout << "Image-only HDL install verified\n"
              << "  partition: " << result.partition_id << '\n'
              << "  startup:   " << result.startup << '\n'
              << "  main LBA:  " << result.main_start_lba << '\n'
              << "  subparts:  " << result.sub_count << '\n'
              << "  payload:   " << result.payload_bytes << " bytes\n"
              << "  allocated: " << result.allocated_bytes << " bytes\n";
    return 0;
}

int patch_metadata(int argc, char** argv)
{
    if (argc < 5) {
        usage();
        return 2;
    }

    const char* image_path = argv[2];
    const std::string partition_id = argv[3];
    ps2hdd::hdl::MetadataPatch patch;
    bool apply = false;

    for (int i = 4; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "--apply") {
            apply = true;
        } else if (option == "--title") {
            if (++i >= argc) {
                std::cerr << "--title requires a value.\n";
                return 2;
            }
            patch.title = argv[i];
        } else if (option == "--compat") {
            if (++i >= argc) {
                std::cerr << "--compat requires a value.\n";
                return 2;
            }
            const auto value = parse_u64(argv[i]);
            if (!value || *value > 0xFFU) {
                std::cerr << "--compat must be in 0..255.\n";
                return 2;
            }
            patch.compat_flags = static_cast<std::uint8_t>(*value);
        } else if (option == "--dma") {
            if (++i >= argc) {
                std::cerr << "--dma requires a value.\n";
                return 2;
            }
            const auto value = parse_u64(argv[i]);
            if (!value || *value > 0xFFFFU) {
                std::cerr << "--dma must be in 0..65535.\n";
                return 2;
            }
            patch.dma = static_cast<std::uint16_t>(*value);
        } else {
            std::cerr << "Unknown patch option: " << option << '\n';
            return 2;
        }
    }

    if (patch.empty()) {
        std::cerr << "No metadata changes were requested.\n";
        return 2;
    }
    if (!apply) {
        std::cerr << "Refusing mutation without explicit --apply.\n";
        return 2;
    }

    ps2hdd::WritableFileBlockDevice image(image_path);
    if (!image.is_open()) {
        std::cerr << "Could not open existing image for explicit read/write access: "
                  << image_path << '\n';
        return 1;
    }

    ps2hdd::apa::Reader reader(image);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        std::cerr << "Refusing metadata mutation because the APA scan is not clean.\n";
        return 1;
    }

    const auto* partition = find_hdl_partition(scan, partition_id);
    if (partition == nullptr) {
        std::cerr << "HDL main partition not found: " << partition_id << '\n';
        return 1;
    }

    const auto before = ps2hdd::hdl::read_game_info(image, *partition);
    if (!before.ok) {
        std::cerr << "Refusing invalid HDL metadata: " << before.error << '\n';
        return 1;
    }

    std::cout << "Applying image-only HDL metadata transaction\n"
              << "  image:      " << image.display_name() << '\n'
              << "  partition:  " << partition->id << '\n'
              << "  old title:  " << before.game.title << '\n'
              << "  old compat: " << static_cast<unsigned>(before.game.compat_flags) << '\n'
              << "  old DMA:    " << before.game.dma << '\n';

    const auto result = ps2hdd::hdl::patch_game_metadata(image, *partition, patch);
    if (!result.ok) {
        std::cerr << "HDL metadata transaction failed: " << result.error << '\n';
        return 1;
    }

    std::cout << "Verified after write:\n"
              << "  title:      " << result.game.title << '\n'
              << "  compat:     " << static_cast<unsigned>(result.game.compat_flags) << '\n'
              << "  DMA:        " << result.game.dma << '\n'
              << "Transaction committed and reparsed successfully.\n";
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
    if (command == "inspect-iso") {
        if (argc != 3) {
            usage();
            return 2;
        }
        return inspect_iso(argv[2]);
    }
    if (command == "plan") {
        if (argc != 4) {
            usage();
            return 2;
        }
        return plan(argv[2], argv[3]);
    }
    if (command == "plan-install") {
        return plan_install(argc, argv);
    }
    if (command == "plan-assets") {
        return plan_assets(argc, argv);
    }
    if (command == "fetch-assets") {
        return fetch_assets(argc, argv);
    }
    if (command == "install-image") {
        return install_image(argc, argv);
    }
    if (command == "patch-metadata") {
        return patch_metadata(argc, argv);
    }

    usage();
    return 2;
}
