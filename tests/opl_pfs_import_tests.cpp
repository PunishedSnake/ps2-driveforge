#include "ps2hdd/opl_pfs_import.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path make_temp_root()
{
    const auto root = std::filesystem::temp_directory_path() / "ps2-driveforge-opl-pfs-import-tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

void write_file(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "open temporary staged file");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(output), "write temporary staged file");
}

ps2hdd::opl::FetchedAsset asset(ps2hdd::opl::AssetKind kind,
                                ps2hdd::opl::Placement placement,
                                std::string target,
                                std::filesystem::path staged,
                                std::string archive_member = {})
{
    ps2hdd::opl::FetchedAsset value;
    value.kind = kind;
    value.placement = placement;
    value.provider = "test";
    value.source_url = "https://example.invalid/test";
    value.target_path = std::move(target);
    value.archive_member = std::move(archive_member);
    value.staged_path = std::move(staged);
    return value;
}

void classic_preflight_builds_one_immutable_batch()
{
    const auto root = make_temp_root();
    const auto cfg = root / "game.cfg";
    const auto art = root / "cover.png";
    const auto catalog = root / "titles.txt";
    const auto icon = root / "icon.sys";
    write_file(cfg, "DMA=7\n");
    write_file(art, "PNGDATA");
    write_file(catalog, "catalog");
    write_file(icon, "hdd-osd");

    std::vector<ps2hdd::opl::FetchedAsset> assets;
    assets.push_back(asset(ps2hdd::opl::AssetKind::cfg, ps2hdd::opl::Placement::opl_data,
                           "CFG/SLUS_123.45.cfg", cfg));
    assets.push_back(asset(ps2hdd::opl::AssetKind::artwork_cover, ps2hdd::opl::Placement::opl_data,
                           "ART/SLUS_123.45_COV.png", art));
    assets.push_back(asset(ps2hdd::opl::AssetKind::title_database, ps2hdd::opl::Placement::cache_only,
                           "", catalog));
    assets.push_back(asset(ps2hdd::opl::AssetKind::hdd_osd_icon,
                           ps2hdd::opl::Placement::hdd_osd_metadata,
                           "PP.SLUS-12345/icon.sys", icon));

    const auto prepared = ps2hdd::opl::prepare_pfs_import(assets);
    check(prepared.ok, "classic OPL preflight succeeds");
    check(prepared.files.size() == 2 && prepared.archives.empty(),
          "only classic OPL PFS placements become batch files");
    check(prepared.cache_only_skipped == 1 && prepared.non_pfs_skipped == 1,
          "non-PFS placements are classified without disk writes");
    check(prepared.issues.size() == 1 && !prepared.issues[0].fatal,
          "HDD-OSD placement produces a non-fatal explicit issue");
    check(prepared.files[0].path == "CFG/SLUS_123.45.cfg" &&
              prepared.files[1].path == "ART/SLUS_123.45_COV.png",
          "PFS destinations are preserved exactly");
    check(prepared.total_bytes == 6 + 7, "preflight accounts exact host bytes");
    check(prepared.files[0].bytes.size() == 6 && prepared.files[1].bytes.size() == 7,
          "host files are frozen into the immutable batch before mutation");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void duplicate_destinations_fail_before_mutation()
{
    const auto root = make_temp_root();
    const auto first = root / "first.cfg";
    const auto second = root / "second.cfg";
    write_file(first, "A=1\n");
    write_file(second, "A=2\n");

    std::vector<ps2hdd::opl::FetchedAsset> assets;
    assets.push_back(asset(ps2hdd::opl::AssetKind::cfg, ps2hdd::opl::Placement::opl_data,
                           "CFG/SLUS_123.45.cfg", first));
    assets.push_back(asset(ps2hdd::opl::AssetKind::cfg, ps2hdd::opl::Placement::opl_data,
                           "CFG/SLUS_123.45.cfg", second));

    const auto prepared = ps2hdd::opl::prepare_pfs_import(assets);
    check(!prepared.ok, "duplicate PFS destinations are rejected");
    check(!prepared.error.empty(), "duplicate preflight has top-level failure reason");
    check(prepared.issues.size() == 1 && prepared.issues[0].fatal,
          "duplicate destination is a fatal preflight issue");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void tar_members_are_grouped_and_validated_before_mutation()
{
    const auto root = make_temp_root();
    const auto cover = root / "cover.png";
    const auto icon = root / "icon.png";
    write_file(cover, "COVER123");
    write_file(icon, "ICON456");

    std::vector<ps2hdd::opl::FetchedAsset> tar_assets;
    tar_assets.push_back(asset(ps2hdd::opl::AssetKind::artwork_cover,
                               ps2hdd::opl::Placement::opl_data,
                               "ART/art.tar", cover, "SLUS_123.45_COV.png"));
    tar_assets.push_back(asset(ps2hdd::opl::AssetKind::artwork_icon,
                               ps2hdd::opl::Placement::opl_data,
                               "ART/art.tar", icon, "SLUS_123.45_ICO.png"));
    const auto prepared = ps2hdd::opl::prepare_pfs_import(tar_assets);
    check(prepared.ok, "TAR-layout OPL preflight succeeds with native updater");
    check(prepared.files.empty() && prepared.archives.size() == 1,
          "members sharing one TAR become one grouped container plan");
    check(prepared.archives[0].path == "ART/art.tar" &&
              prepared.archives[0].members.size() == 2,
          "TAR group preserves destination and distinct member count");
    check(prepared.directories.size() == 1 && prepared.directories[0] == "ART",
          "TAR container parent directory is included in mkdir plan");
    check(prepared.total_bytes == 15, "TAR preflight accounts staged member bytes");

    std::vector<ps2hdd::opl::FetchedAsset> duplicate;
    duplicate.push_back(asset(ps2hdd::opl::AssetKind::artwork_cover,
                              ps2hdd::opl::Placement::opl_data,
                              "ART/art.tar", cover, "same.png"));
    duplicate.push_back(asset(ps2hdd::opl::AssetKind::artwork_icon,
                              ps2hdd::opl::Placement::opl_data,
                              "ART/art.tar", icon, "same.png"));
    const auto duplicate_preflight = ps2hdd::opl::prepare_pfs_import(duplicate);
    check(!duplicate_preflight.ok,
          "duplicate member inside one TAR is rejected before mutation");

    std::vector<ps2hdd::opl::FetchedAsset> collision;
    collision.push_back(asset(ps2hdd::opl::AssetKind::artwork_cover,
                              ps2hdd::opl::Placement::opl_data,
                              "ART/art.tar", cover));
    collision.push_back(asset(ps2hdd::opl::AssetKind::artwork_icon,
                              ps2hdd::opl::Placement::opl_data,
                              "ART/art.tar", icon, "inside.png"));
    check(!ps2hdd::opl::prepare_pfs_import(collision).ok,
          "classic file cannot collide with TAR container destination");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void size_limit_fails_closed()
{
    const auto root = make_temp_root();
    const auto file = root / "cover.png";
    write_file(file, "12345678");

    ps2hdd::opl::PfsImportOptions limited;
    limited.max_file_bytes = 4;
    std::vector<ps2hdd::opl::FetchedAsset> large_assets;
    large_assets.push_back(asset(ps2hdd::opl::AssetKind::artwork_cover,
                                 ps2hdd::opl::Placement::opl_data,
                                 "ART/SLUS_123.45_COV.png", file));
    const auto large = ps2hdd::opl::prepare_pfs_import(large_assets, limited);
    check(!large.ok && !large.issues.empty() && large.issues[0].fatal,
          "per-file byte limit is enforced during preflight");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main()
{
    try {
        classic_preflight_builds_one_immutable_batch();
        duplicate_destinations_fail_before_mutation();
        tar_members_are_grouped_and_validated_before_mutation();
        size_limit_fails_closed();
        std::cout << "OPL PFS import preflight tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OPL PFS import test failure: " << error.what() << '\n';
        return 1;
    }
}
