#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd::opl {

enum class AssetKind {
    title_database,
    redump_database,
    cfg,
    widescreen_cht,
    artwork_cover,
    artwork_icon,
    artwork_background,
    artwork_screenshot,
    artwork_logo,
    artwork_label,
    hdd_osd_icon,
};

enum class Placement {
    cache_only,
    opl_data,
    hdd_osd_metadata,
};

enum class MergePolicy {
    first_available,
    merge_cfg_overlay,
    merge_mastercode_if_needed,
};

enum class LayoutMode {
    classic_files,
    tar_archives,
};

struct GameIdentity {
    std::string startup;
    std::string title;
};

struct AssetOptions {
    bool title_database{true};
    bool redump_database{true};
    bool cfg_metadata{true};
    bool cfg_compatibility{true};
    bool widescreen{true};
    bool verified_mastercode{true};
    bool artwork_cover{true};
    bool artwork_icon{true};
    bool artwork_background{true};
    bool artwork_screenshot{true};
    bool artwork_logo{};
    bool artwork_label{};
    bool hdd_osd_icon{};
    LayoutMode layout{LayoutMode::classic_files};
};

struct AssetCandidate {
    std::string provider;
    std::string url;
    std::string target_path;
    std::string archive_member;
};

struct AssetRequest {
    AssetKind kind{};
    Placement placement{};
    MergePolicy merge_policy{MergePolicy::first_available};
    bool optional{true};
    std::vector<AssetCandidate> candidates;
};

struct AssetPlan {
    bool ok{};
    std::string game_id;
    std::string title;
    std::vector<AssetRequest> requests;
    std::string error;
};

// Converts common PS2 executable spellings to the OPL serial form used by ART,
// CFG and CHT databases, e.g. SLUS-20946 -> SLUS_209.46. Returns nullopt when
// the input cannot be represented without guessing.
[[nodiscard]] std::optional<std::string> canonical_game_id(std::string_view startup);

[[nodiscard]] AssetPlan plan_assets(const GameIdentity& game,
                                    const AssetOptions& options = {});

[[nodiscard]] bool cht_has_mastercode(std::string_view text) noexcept;

// Adds a verified bare mastercode only when the primary widescreen CHT does not
// already contain one. The fallback file's title/header is deliberately not
// duplicated; only its Mastercode section is appended.
[[nodiscard]] std::string merge_cht_with_mastercode(std::string_view primary,
                                                     std::string_view mastercode);

// Merges OPL key=value configuration while preserving the base file's ordering
// and comments. Overlay keys replace matching base keys and new overlay keys are
// appended. This is intended for the small HDD compatibility overlay database.
[[nodiscard]] std::string merge_cfg_overlay(std::string_view base,
                                            std::string_view overlay);

[[nodiscard]] std::string_view asset_kind_name(AssetKind kind) noexcept;

} // namespace ps2hdd::opl
