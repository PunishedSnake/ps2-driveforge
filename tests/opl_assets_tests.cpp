#include "ps2hdd/opl_assets.hpp"
#include "ps2hdd/opl_metadata.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

const ps2hdd::opl::AssetRequest* find_request(const ps2hdd::opl::AssetPlan& plan,
                                               ps2hdd::opl::AssetKind kind)
{
    for (const auto& request : plan.requests) {
        if (request.kind == kind) return &request;
    }
    return nullptr;
}

void test_game_id_normalization()
{
    using ps2hdd::opl::canonical_game_id;
    check(canonical_game_id("SLUS_209.46") == "SLUS_209.46", "canonical ID changed");
    check(canonical_game_id("slus-20946") == "SLUS_209.46", "HDL-style ID did not normalize");
    check(canonical_game_id("SLUS20946") == "SLUS_209.46", "compact ID did not normalize");
    check(canonical_game_id("cdrom0:\\SLUS_209.46;1") == "SLUS_209.46", "SYSTEM.CNF path did not normalize");
    check(!canonical_game_id("this-is-not-a-serial"), "invalid ID was accepted");
}

void test_default_plan()
{
    const auto plan = ps2hdd::opl::plan_assets({"SLUS_209.46", "Grand Theft Auto: San Andreas"});
    check(plan.ok, "default asset plan failed");
    check(plan.game_id == "SLUS_209.46", "plan game ID mismatch");

    const auto* cfg = find_request(plan, ps2hdd::opl::AssetKind::cfg);
    check(cfg != nullptr, "default CFG request missing");
    check(cfg->merge_policy == ps2hdd::opl::MergePolicy::merge_cfg_overlay, "CFG request is not overlay-aware");
    check(cfg->candidates.size() == 2, "CFG providers were not both planned");
    check(cfg->candidates[0].url.find("israpps/PS2-OPL-CFG-Database") != std::string::npos, "base CFG provider mismatch");
    check(cfg->candidates[1].url.find("GDX-X/PS2-OPL-CFG-Compatibility-Database") != std::string::npos, "compatibility CFG provider mismatch");

    const auto* cht = find_request(plan, ps2hdd::opl::AssetKind::widescreen_cht);
    check(cht != nullptr, "default widescreen request missing");
    check(cht->merge_policy == ps2hdd::opl::MergePolicy::merge_mastercode_if_needed, "widescreen request does not use mastercode fallback");
    check(cht->candidates.size() == 2, "mastercode fallback was not planned");
    check(cht->candidates[0].target_path == "CHT/SLUS_209.46.cht", "classic CHT destination mismatch");

    const auto* art = find_request(plan, ps2hdd::opl::AssetKind::artwork_cover);
    check(art != nullptr && art->candidates.size() == 2, "cover providers missing");
    check(art->candidates[0].target_path == "ART/SLUS_209.46_COV.png", "PNG cover destination mismatch");
    check(art->candidates[1].target_path == "ART/SLUS_209.46_COV.jpg", "archive cover fallback destination mismatch");
}

void test_tar_plan()
{
    ps2hdd::opl::AssetOptions options;
    options.layout = ps2hdd::opl::LayoutMode::tar_archives;
    options.redump_database = false;
    const auto plan = ps2hdd::opl::plan_assets({"SCES_503.61", "Test"}, options);
    check(plan.ok, "tar asset plan failed");

    const auto* cfg = find_request(plan, ps2hdd::opl::AssetKind::cfg);
    check(cfg != nullptr, "tar CFG request missing");
    check(cfg->candidates[0].target_path == "CFG/cfg.tar", "tar CFG path mismatch");
    check(cfg->candidates[0].archive_member == "SCES_503.61.cfg", "tar CFG member mismatch");

    const auto* cht = find_request(plan, ps2hdd::opl::AssetKind::widescreen_cht);
    check(cht != nullptr, "tar CHT request missing");
    check(cht->candidates[0].target_path == "CHT/cht.tar", "tar CHT path mismatch");
    check(cht->candidates[0].archive_member == "SCES_503.61.cht", "tar CHT member mismatch");

    const auto* art = find_request(plan, ps2hdd::opl::AssetKind::artwork_background);
    check(art != nullptr, "tar ART request missing");
    check(art->candidates[0].target_path == "ART/art.tar", "tar ART path mismatch");
    check(art->candidates[0].archive_member == "SCES_503.61_BG_00.png", "tar ART member mismatch");
}

void test_mastercode_merge()
{
    constexpr std::string_view widescreen =
        "\"Example /ID SLUS_000.00\"\n"
        "// Widescreen\n"
        "20100000 3c013f40\n";
    constexpr std::string_view fallback =
        "\"Example /ID SLUS_000.00\"\n"
        "// ELF CRC 12345678\n"
        "Mastercode\n"
        "90123456 0C123456\n";

    check(!ps2hdd::opl::cht_has_mastercode(widescreen), "false mastercode positive");
    const auto merged = ps2hdd::opl::merge_cht_with_mastercode(widescreen, fallback);
    check(ps2hdd::opl::cht_has_mastercode(merged), "fallback mastercode was not merged");
    check(merged.find("90123456 0C123456") != std::string::npos, "mastercode payload missing after merge");
    check(merged.find("ELF CRC 12345678") == std::string::npos, "fallback title/header material leaked into merged CHT");

    constexpr std::string_view already =
        "\"Example /ID SLUS_000.00\"\nMastercode\n90000000 00000000\n";
    check(ps2hdd::opl::merge_cht_with_mastercode(already, fallback) == already, "existing mastercode should remain untouched");
}

void test_cfg_overlay()
{
    constexpr std::string_view base =
        "Title=Example\n"
        "$Compatibility=0x01\n"
        "Developer=Someone\n";
    constexpr std::string_view overlay =
        "$Compatibility=0x20\n"
        "$ConfigSource=DriveForgeTest\n";

    const auto merged = ps2hdd::opl::merge_cfg_overlay(base, overlay);
    check(merged.find("$Compatibility=0x20") != std::string::npos, "compatibility key did not override base");
    check(merged.find("$Compatibility=0x01") == std::string::npos, "old compatibility key survived overlay");
    check(merged.find("Title=Example") != std::string::npos, "base metadata was lost");
    check(merged.find("$ConfigSource=DriveForgeTest") != std::string::npos, "new overlay key was not appended");
}

void test_automatic_media_detection()
{
    using ps2hdd::hdl::MediaType;
    check(ps2hdd::opl::automatic_media_type(650ULL * 1024ULL * 1024ULL) == MediaType::cd, "normal CD image was not detected as CD");
    check(ps2hdd::opl::automatic_media_type(ps2hdd::opl::kAutomaticCdImageCeiling) == MediaType::cd, "CD ceiling should remain CD");
    check(ps2hdd::opl::automatic_media_type(ps2hdd::opl::kAutomaticCdImageCeiling + 1ULL) == MediaType::dvd, "image above CD ceiling was not detected as DVD");
    check(ps2hdd::opl::automatic_media_type(4ULL * 1024ULL * 1024ULL * 1024ULL) == MediaType::dvd, "DVD image was not detected as DVD");
}

void test_gamename_database_lookup()
{
    constexpr std::string_view csv =
        "SLPS_255.31;SD Gundam G Generation - Gundam Seed Edition\r\n"
        "SLPS_255.32;Critical Velocity\r\n"
        "SLPS_255.33;Tales of Legendia\r\n";
    const auto found = ps2hdd::opl::lookup_title_database(csv, "slps_255.32");
    check(found && *found == "Critical Velocity", "gamename.csv exact serial lookup failed");
    check(!ps2hdd::opl::lookup_title_database(csv, "SLPS_255.34"), "missing serial produced a title");
}

void test_cfg_metadata_parse()
{
    constexpr std::string_view cfg =
        "CfgVersion=8\n"
        "Title=Critical Velocity\n"
        "Developer=Namco\n"
        "Genre=Racing\n"
        "Players=1-2\n"
        "Release=2005-10-13\n"
        "Rating=4.0\n"
        "Description=Test metadata\n";
    const auto metadata = ps2hdd::opl::parse_cfg_metadata(cfg);
    check(metadata.cfg_found, "CFG metadata was not marked present");
    check(metadata.title == "Critical Velocity", "CFG title parse failed");
    check(metadata.developer == "Namco", "CFG developer parse failed");
    check(metadata.genre == "Racing", "CFG genre parse failed");
    check(metadata.players == "1-2", "CFG players parse failed");
    check(metadata.release_date == "2005-10-13", "CFG release parse failed");
    check(metadata.rating == "4.0", "CFG rating parse failed");
    check(metadata.description == "Test metadata", "CFG description parse failed");
}

} // namespace

int main()
{
    try {
        test_game_id_normalization();
        test_default_plan();
        test_tar_plan();
        test_mastercode_merge();
        test_cfg_overlay();
        test_automatic_media_detection();
        test_gamename_database_lookup();
        test_cfg_metadata_parse();
        std::cout << "OPL asset tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OPL asset test failure: " << error.what() << '\n';
        return 1;
    }
}
