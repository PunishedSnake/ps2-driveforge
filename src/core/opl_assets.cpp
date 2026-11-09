#include "ps2hdd/opl_assets.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace ps2hdd::opl {
namespace {

std::string trim(std::string_view value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string upper_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool all_digits(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

bool valid_prefix(std::string_view value)
{
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0;
    });
}

std::pair<std::string, std::string> opl_target(LayoutMode layout,
                                                std::string_view directory,
                                                std::string filename)
{
    if (layout == LayoutMode::classic_files) {
        return {std::string(directory) + "/" + filename, {}};
    }

    std::string archive;
    if (directory == "ART") {
        archive = "ART/art.tar";
    } else if (directory == "CFG") {
        archive = "CFG/cfg.tar";
    } else if (directory == "CHT") {
        archive = "CHT/cht.tar";
    } else {
        return {std::string(directory) + "/" + filename, {}};
    }
    return {std::move(archive), std::move(filename)};
}

AssetCandidate candidate(std::string provider, std::string url,
                         std::pair<std::string, std::string> target)
{
    return {std::move(provider), std::move(url), std::move(target.first), std::move(target.second)};
}

void add_art_request(AssetPlan& plan, AssetKind kind, LayoutMode layout,
                     std::string_view game_id, std::string_view primary_suffix,
                     std::string_view fallback_suffix)
{
    AssetRequest request;
    request.kind = kind;
    request.placement = Placement::opl_data;
    request.merge_policy = MergePolicy::first_available;
    request.optional = true;

    const std::string primary_name = std::string(game_id) + std::string(primary_suffix);
    request.candidates.push_back(candidate(
        "Luden02/psx-ps2-opl-art-database",
        "https://raw.githubusercontent.com/Luden02/psx-ps2-opl-art-database/main/PS2/" +
            std::string(game_id) + "/" + primary_name,
        opl_target(layout, "ART", primary_name)));

    if (!fallback_suffix.empty()) {
        const std::string fallback_name = std::string(game_id) + std::string(fallback_suffix);
        request.candidates.push_back(candidate(
            "OPLM_ART_2023_07 archive mirror",
            "https://archive.org/download/OPLM_ART_2023_07/OPLM_ART_2023_07.zip/PS2%2F" +
                std::string(game_id) + "%2F" + fallback_name,
            opl_target(layout, "ART", fallback_name)));
    }

    plan.requests.push_back(std::move(request));
}

std::vector<std::string> lines(std::string_view text)
{
    std::vector<std::string> result;
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const auto end = text.find('\n', cursor);
        const auto length = end == std::string_view::npos ? text.size() - cursor : end - cursor;
        std::string line(text.substr(cursor, length));
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        result.push_back(std::move(line));
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 1;
    }
    return result;
}

std::optional<std::string> cfg_key(std::string_view line)
{
    const std::string clean = trim(line);
    if (clean.empty() || clean[0] == '#' || clean[0] == ';' || clean.rfind("//", 0) == 0) {
        return std::nullopt;
    }
    const auto equals = clean.find('=');
    if (equals == std::string::npos) {
        return std::nullopt;
    }
    const std::string key = trim(std::string_view(clean).substr(0, equals));
    if (key.empty()) {
        return std::nullopt;
    }
    return key;
}

std::string join_lines(const std::vector<std::string>& input)
{
    std::string output;
    for (std::size_t i = 0; i < input.size(); ++i) {
        output += input[i];
        if (i + 1 < input.size() || !input.empty()) {
            output += '\n';
        }
    }
    return output;
}

} // namespace

std::optional<std::string> canonical_game_id(std::string_view startup)
{
    std::string value = trim(startup);
    if (value.empty()) {
        return std::nullopt;
    }

    if (value.front() == '"' && value.size() >= 2 && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    const auto semicolon = value.find(';');
    if (semicolon != std::string::npos) {
        value.resize(semicolon);
    }
    const auto separator = value.find_last_of("\\/:");
    if (separator != std::string::npos) {
        value = value.substr(separator + 1);
    }
    value = upper_ascii(trim(value));

    if (value.size() == 11 && value[4] == '_' && value[8] == '.' &&
        valid_prefix(std::string_view(value).substr(0, 4)) &&
        all_digits(std::string_view(value).substr(5, 3)) &&
        all_digits(std::string_view(value).substr(9, 2))) {
        return value;
    }

    if (value.size() == 10 && value[4] == '-' &&
        valid_prefix(std::string_view(value).substr(0, 4)) &&
        all_digits(std::string_view(value).substr(5, 5))) {
        return value.substr(0, 4) + "_" + value.substr(5, 3) + "." + value.substr(8, 2);
    }

    if (value.size() == 9 && valid_prefix(std::string_view(value).substr(0, 4)) &&
        all_digits(std::string_view(value).substr(4, 5))) {
        return value.substr(0, 4) + "_" + value.substr(4, 3) + "." + value.substr(7, 2);
    }

    return std::nullopt;
}

AssetPlan plan_assets(const GameIdentity& game, const AssetOptions& options)
{
    AssetPlan plan;
    const auto game_id = canonical_game_id(game.startup);
    if (!game_id) {
        plan.error = "Could not normalize PS2 startup ID for OPL asset lookup";
        return plan;
    }
    plan.game_id = *game_id;
    plan.title = game.title;

    if (options.title_database) {
        AssetRequest request;
        request.kind = AssetKind::title_database;
        request.placement = Placement::cache_only;
        request.optional = true;
        request.candidates.push_back({
            "israpps/HDL-Batch-installer",
            "https://raw.githubusercontent.com/israpps/HDL-Batch-installer/main/Database/gamename.csv",
            "catalog/gamename.csv",
            {},
        });
        plan.requests.push_back(std::move(request));
    }

    if (options.redump_database) {
        AssetRequest request;
        request.kind = AssetKind::redump_database;
        request.placement = Placement::cache_only;
        request.optional = true;
        request.candidates.push_back({
            "israpps/HDL-Batch-installer",
            "https://raw.githubusercontent.com/israpps/HDL-Batch-installer/main/Database/redump.csv",
            "catalog/redump.csv",
            {},
        });
        plan.requests.push_back(std::move(request));
    }

    if (options.cfg_metadata || options.cfg_compatibility) {
        AssetRequest request;
        request.kind = AssetKind::cfg;
        request.placement = Placement::opl_data;
        request.optional = true;
        request.merge_policy = options.cfg_metadata && options.cfg_compatibility
                                   ? MergePolicy::merge_cfg_overlay
                                   : MergePolicy::first_available;
        const auto target = opl_target(options.layout, "CFG", plan.game_id + ".cfg");
        if (options.cfg_metadata) {
            request.candidates.push_back(candidate(
                "israpps/PS2-OPL-CFG-Database",
                "https://raw.githubusercontent.com/israpps/PS2-OPL-CFG-Database/master/CFG_en/" +
                    plan.game_id + ".cfg",
                target));
        }
        if (options.cfg_compatibility) {
            request.candidates.push_back(candidate(
                "GDX-X/PS2-OPL-CFG-Compatibility-Database",
                "https://raw.githubusercontent.com/GDX-X/PS2-OPL-CFG-Compatibility-Database/main/HDD/" +
                    plan.game_id + ".cfg",
                target));
        }
        plan.requests.push_back(std::move(request));
    }

    if (options.widescreen) {
        AssetRequest request;
        request.kind = AssetKind::widescreen_cht;
        request.placement = Placement::opl_data;
        request.optional = true;
        request.merge_policy = options.verified_mastercode
                                   ? MergePolicy::merge_mastercode_if_needed
                                   : MergePolicy::first_available;
        const auto target = opl_target(options.layout, "CHT", plan.game_id + ".cht");
        request.candidates.push_back(candidate(
            "PS2-Widescreen/OPL-Widescreen-Cheats",
            "https://raw.githubusercontent.com/PS2-Widescreen/OPL-Widescreen-Cheats/main/CHT/" +
                plan.game_id + ".cht",
            target));
        if (options.verified_mastercode) {
            request.candidates.push_back(candidate(
                "PS2-Widescreen/Bare-Mastercodes-bin",
                "https://raw.githubusercontent.com/PS2-Widescreen/Bare-Mastercodes-bin/main/MASTERCODES/" +
                    plan.game_id + ".cht",
                target));
        }
        plan.requests.push_back(std::move(request));
    }

    if (options.artwork_cover) {
        add_art_request(plan, AssetKind::artwork_cover, options.layout, plan.game_id,
                        "_COV.png", "_COV.jpg");
    }
    if (options.artwork_icon) {
        add_art_request(plan, AssetKind::artwork_icon, options.layout, plan.game_id,
                        "_ICO.png", "_ICO.png");
    }
    if (options.artwork_background) {
        add_art_request(plan, AssetKind::artwork_background, options.layout, plan.game_id,
                        "_BG_00.png", "_BG_00.jpg");
    }
    if (options.artwork_screenshot) {
        add_art_request(plan, AssetKind::artwork_screenshot, options.layout, plan.game_id,
                        "_SCR_00.png", "_SCR_00.jpg");
    }
    if (options.artwork_logo) {
        add_art_request(plan, AssetKind::artwork_logo, options.layout, plan.game_id,
                        "_LGO.png", "_LGO.png");
    }
    if (options.artwork_label) {
        add_art_request(plan, AssetKind::artwork_label, options.layout, plan.game_id,
                        "_LAB.png", "_LAB.jpg");
    }

    if (options.hdd_osd_icon) {
        AssetRequest request;
        request.kind = AssetKind::hdd_osd_icon;
        request.placement = Placement::hdd_osd_metadata;
        request.optional = true;
        request.candidates.push_back({
            "CosmicScale/HDD-OSD-Icon-Database",
            "https://raw.githubusercontent.com/CosmicScale/HDD-OSD-Icon-Database/main/ico/" +
                plan.game_id + ".ico",
            "HDD-OSD/" + plan.game_id + ".ico",
            {},
        });
        plan.requests.push_back(std::move(request));
    }

    plan.ok = true;
    return plan;
}

bool cht_has_mastercode(std::string_view text) noexcept
{
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const auto end = text.find('\n', cursor);
        const auto length = end == std::string_view::npos ? text.size() - cursor : end - cursor;
        std::string line = trim(text.substr(cursor, length));
        line = upper_ascii(std::move(line));
        if (line == "MASTERCODE") {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 1;
    }
    return false;
}

std::string merge_cht_with_mastercode(std::string_view primary, std::string_view mastercode)
{
    if (primary.empty() || mastercode.empty() || cht_has_mastercode(primary)) {
        return std::string(primary);
    }

    const auto fallback_lines = lines(mastercode);
    std::size_t master_index = fallback_lines.size();
    for (std::size_t i = 0; i < fallback_lines.size(); ++i) {
        if (upper_ascii(trim(fallback_lines[i])) == "MASTERCODE") {
            master_index = i;
            break;
        }
    }
    if (master_index == fallback_lines.size()) {
        return std::string(primary);
    }

    std::string output(primary);
    if (!output.empty() && output.back() != '\n') {
        output += '\n';
    }
    output += "\n// DriveForge: verified fallback mastercode\n";
    for (std::size_t i = master_index; i < fallback_lines.size(); ++i) {
        output += fallback_lines[i];
        output += '\n';
    }
    return output;
}

std::string merge_cfg_overlay(std::string_view base, std::string_view overlay)
{
    if (base.empty()) {
        return std::string(overlay);
    }
    if (overlay.empty()) {
        return std::string(base);
    }

    const auto base_lines = lines(base);
    const auto overlay_lines = lines(overlay);
    std::map<std::string, std::string> replacements;
    std::vector<std::string> overlay_order;
    for (const auto& line : overlay_lines) {
        const auto key = cfg_key(line);
        if (!key) {
            continue;
        }
        if (!replacements.contains(*key)) {
            overlay_order.push_back(*key);
        }
        replacements[*key] = line;
    }

    std::set<std::string> consumed;
    std::vector<std::string> merged;
    merged.reserve(base_lines.size() + overlay_order.size() + 2);
    for (const auto& line : base_lines) {
        const auto key = cfg_key(line);
        if (key) {
            const auto replacement = replacements.find(*key);
            if (replacement != replacements.end()) {
                merged.push_back(replacement->second);
                consumed.insert(*key);
                continue;
            }
        }
        merged.push_back(line);
    }

    bool added_header = false;
    for (const auto& key : overlay_order) {
        if (consumed.contains(key)) {
            continue;
        }
        if (!added_header) {
            if (!merged.empty() && !merged.back().empty()) {
                merged.emplace_back();
            }
            merged.emplace_back("# DriveForge compatibility overlay");
            added_header = true;
        }
        merged.push_back(replacements.at(key));
    }

    return join_lines(merged);
}

std::string_view asset_kind_name(AssetKind kind) noexcept
{
    switch (kind) {
    case AssetKind::title_database: return "title database";
    case AssetKind::redump_database: return "Redump database";
    case AssetKind::cfg: return "OPL CFG";
    case AssetKind::widescreen_cht: return "widescreen CHT";
    case AssetKind::artwork_cover: return "cover artwork";
    case AssetKind::artwork_icon: return "icon artwork";
    case AssetKind::artwork_background: return "background artwork";
    case AssetKind::artwork_screenshot: return "screenshot artwork";
    case AssetKind::artwork_logo: return "logo artwork";
    case AssetKind::artwork_label: return "label artwork";
    case AssetKind::hdd_osd_icon: return "HDD-OSD icon";
    }
    return "unknown asset";
}

} // namespace ps2hdd::opl
