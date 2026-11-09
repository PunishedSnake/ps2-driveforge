#pragma once

#include "ps2hdd/hdl.hpp"
#include "ps2hdd/http_client.hpp"
#include "ps2hdd/opl_asset_pipeline.hpp"
#include "ps2hdd/opl_assets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd::bdm {

// OPL "BDM" game libraries: a FAT32/exFAT root (the exFAT partition of an
// APA-Jail drive, a USB stick, MX4SIO, iLink) holding plain ISO files and the
// usual OPL resource folders. The host OS owns the filesystem; DriveForge adds
// what OPL needs on top of it: the folder layout, contiguous game images
// (OPL reads raw extents and tolerates only limited fragmentation), stable
// file names and the same asset pipeline used for +OPL.

inline constexpr std::array<std::string_view, 10> kLayoutDirectories{
    "DVD", "CD", "ART", "CFG", "CHT", "VMC", "APPS", "THM", "LNG", "POPS"};
// OPL's historical BDM name limit (ID prefix + title + extension).
inline constexpr std::size_t kMaxGameFileName = 64;
// Recent OPL builds follow up to this many fragments per image; one is ideal.
inline constexpr std::size_t kOplMaxFragments = 64;

struct LayoutResult {
    bool ok{};
    std::string error;
    std::vector<std::string> created;
};
[[nodiscard]] LayoutResult ensure_layout(const std::filesystem::path& root);

struct FragmentReport {
    bool known{};          // platform could report extents for this file
    std::size_t extents{}; // 1 = contiguous
};
[[nodiscard]] FragmentReport count_fragments(const std::filesystem::path& file);

// "SLUS_123.45.Title.iso": the ID-prefixed form every OPL BDM build parses
// without opening the image; FAT/exFAT-safe and capped at kMaxGameFileName.
[[nodiscard]] std::string game_file_name(std::string_view startup, std::string_view title,
                                         std::string_view extension = ".iso");

struct InstallOptions {
    std::string title;                   // default: ISO file stem
    std::optional<hdl::MediaType> media; // default: <= 750 MiB -> CD, else DVD
    bool overwrite{false};
    std::size_t chunk_bytes{8U * 1024U * 1024U};
};

struct InstallResult {
    bool ok{};
    bool cancelled{};
    std::string error;
    std::filesystem::path path;
    std::string startup;
    hdl::MediaType media{hdl::MediaType::dvd};
    std::uint64_t bytes{};
    FragmentReport fragments;
    std::vector<std::string> warnings;
};

using Progress = std::function<void(std::uint64_t done, std::uint64_t total)>;

// Copies a PS2 ISO into <root>/DVD or <root>/CD. The full size is reserved
// before the first byte is written so the filesystem can hand out one run,
// data goes to "<name>.partial" and is renamed only after a complete flush,
// and the result's fragmentation is measured and reported.
[[nodiscard]] InstallResult install_game(const std::filesystem::path& root,
                                         const std::filesystem::path& iso,
                                         const InstallOptions& options = {},
                                         Progress progress = {}, std::stop_token stop = {});

struct LibraryGame {
    std::filesystem::path path;
    std::string startup; // canonical OPL ID when known
    std::string title;
    bool cd{};
    std::uint64_t bytes{};
    FragmentReport fragments;
    std::string error;
};
[[nodiscard]] std::vector<LibraryGame> scan_library(const std::filesystem::path& root,
                                                    bool measure_fragments = true);

struct AssetSyncResult {
    bool ok{};
    std::string error;
    std::size_t games{};
    std::size_t already_present{};
    std::size_t staged{};
    std::size_t written{};
    std::vector<std::string> issues;
};

// Same rules as the +OPL library sync: only missing ART/CFG/CHT are fetched,
// matched by "DIR/<name without extension>" so user files always win; with
// apply=false nothing is written to the library.
[[nodiscard]] AssetSyncResult sync_assets(const std::filesystem::path& root, HttpClient& http,
                                          const std::filesystem::path& cache_root, bool apply,
                                          opl::AssetOptions assets = {}, opl::FetchOptions fetch = {});

} // namespace ps2hdd::bdm
