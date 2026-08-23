#pragma once

#include "ps2hdd/hdl.hpp"
#include "ps2hdd/opl_asset_pipeline.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace ps2hdd::opl {

// A normal PS2 CD image is comfortably below this bound. The threshold is
// intentionally a little generous so dumps with harmless padding still select
// CD automatically. Very small DVD images remain an edge case and can be
// overridden from DriveForge's Advanced install options.
inline constexpr std::uint64_t kAutomaticCdImageCeiling = 800ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr hdl::MediaType automatic_media_type(std::uint64_t image_bytes) noexcept
{
    return image_bytes <= kAutomaticCdImageCeiling ? hdl::MediaType::cd : hdl::MediaType::dvd;
}

struct GameMetadata {
    std::string title;
    std::string description;
    std::string genre;
    std::string release_date;
    std::string developer;
    std::string players;
    std::string rating;
    bool title_from_database{};
    bool cfg_found{};
};

namespace metadata_detail {

[[nodiscard]] inline std::string trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

[[nodiscard]] inline std::string upper_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

[[nodiscard]] inline std::optional<std::string> read_text_bounded(
    const std::filesystem::path& path, std::uint64_t max_bytes = 24ULL * 1024ULL * 1024ULL)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > max_bytes) return std::nullopt;

    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size()))) return std::nullopt;
    return text;
}

inline void assign_cfg_field(GameMetadata& result, std::string key, std::string value)
{
    key = upper_ascii(trim(key));
    value = trim(value);
    if (value.empty()) return;

    if (key == "TITLE") result.title = std::move(value);
    else if (key == "DESCRIPTION") result.description = std::move(value);
    else if (key == "GENRE") result.genre = std::move(value);
    else if (key == "RELEASE" || key == "RELEASEDATE" || key == "RELEASE_DATE") result.release_date = std::move(value);
    else if (key == "DEVELOPER") result.developer = std::move(value);
    else if (key == "PLAYERS") result.players = std::move(value);
    else if (key == "RATING") result.rating = std::move(value);
}

} // namespace metadata_detail

// HDL Batch Installer's gamename.csv is GAME_ID;Title. Match the exact
// canonical OPL serial so one regional release cannot silently lend its title
// to another serial merely because their filenames look friendly.
[[nodiscard]] inline std::optional<std::string> lookup_title_database(
    std::string_view csv, std::string_view canonical_game_id)
{
    const std::string wanted = metadata_detail::upper_ascii(std::string(canonical_game_id));
    std::size_t cursor = 0;
    while (cursor <= csv.size()) {
        const auto end = csv.find('\n', cursor);
        auto line = csv.substr(cursor, end == std::string_view::npos ? csv.size() - cursor : end - cursor);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (cursor == 0 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf) {
            line.remove_prefix(3);
        }
        const auto semi = line.find(';');
        if (semi != std::string_view::npos) {
            const auto id = metadata_detail::upper_ascii(metadata_detail::trim(line.substr(0, semi)));
            if (id == wanted) {
                const auto title = metadata_detail::trim(line.substr(semi + 1));
                if (!title.empty() && title != "!unknown!") return title;
            }
        }
        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }
    return std::nullopt;
}

[[nodiscard]] inline GameMetadata parse_cfg_metadata(std::string_view text)
{
    GameMetadata result;
    result.cfg_found = !text.empty();
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const auto end = text.find('\n', cursor);
        auto line = text.substr(cursor, end == std::string_view::npos ? text.size() - cursor : end - cursor);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto clean = metadata_detail::trim(line);
        if (!clean.empty() && clean.front() != '#' && clean.front() != ';') {
            const auto equals = clean.find('=');
            if (equals != std::string::npos) {
                metadata_detail::assign_cfg_field(result, clean.substr(0, equals), clean.substr(equals + 1));
            }
        }
        if (end == std::string_view::npos) break;
        cursor = end + 1;
    }
    return result;
}

// Resolve metadata only from host-staged provider outputs. This function never
// performs network I/O and never receives a writable disk capability.
[[nodiscard]] inline GameMetadata resolve_game_metadata(
    std::string_view canonical_game_id,
    std::span<const FetchedAsset> assets,
    std::string fallback_title = {})
{
    GameMetadata result;
    result.title = std::move(fallback_title);
    std::optional<std::string> database_title;

    for (const auto& asset : assets) {
        if (asset.kind == AssetKind::title_database) {
            if (const auto text = metadata_detail::read_text_bounded(asset.staged_path)) {
                database_title = lookup_title_database(*text, canonical_game_id);
            }
        } else if (asset.kind == AssetKind::cfg) {
            if (const auto text = metadata_detail::read_text_bounded(asset.staged_path, 2ULL * 1024ULL * 1024ULL)) {
                auto cfg = parse_cfg_metadata(*text);
                result.cfg_found = cfg.cfg_found;
                if (!cfg.description.empty()) result.description = std::move(cfg.description);
                if (!cfg.genre.empty()) result.genre = std::move(cfg.genre);
                if (!cfg.release_date.empty()) result.release_date = std::move(cfg.release_date);
                if (!cfg.developer.empty()) result.developer = std::move(cfg.developer);
                if (!cfg.players.empty()) result.players = std::move(cfg.players);
                if (!cfg.rating.empty()) result.rating = std::move(cfg.rating);
                if (result.title.empty() && !cfg.title.empty()) result.title = std::move(cfg.title);
            }
        }
    }

    // Prefer the same gamename database HDL Batch Installer uses. CFG title is a
    // useful fallback, but gamename.csv is the established installation-title
    // source and avoids filenames becoming user-visible metadata by accident.
    if (database_title) {
        result.title = *database_title;
        result.title_from_database = true;
    }
    return result;
}

} // namespace ps2hdd::opl
