#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/block_device.hpp"
#include "ps2hdd/http_client.hpp"
#include "ps2hdd/opl_asset_pipeline.hpp"
#include "ps2hdd/opl_assets.hpp"
#include "ps2hdd/opl_pfs_import.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

namespace ps2hdd::opl {

// OPL library sync: bring ART/CFG/CHT up to date for HDL games that are
// already installed, using the identity stored in each game's own HDL header.
//
// Two phases, mirroring game install:
//   plan_library_sync()  read-only. Reads HDL headers, resolves the OPL data
//                        partition, inventories what already exists there and
//                        fetches only what is missing into host staging.
//   apply_library_sync() one guarded PFS import of exactly the staged plan,
//                        after re-checking that the OPL partition has not moved.
//
// By default user files win: a game with any ART/<ID>_COV.* already present is
// not given a database cover, and an existing CFG/<ID>.cfg is never replaced.

struct LibrarySyncOptions {
    // Title/Redump catalogs and HDD-OSD icons are not OPL data and are always
    // disabled for library sync regardless of these flags.
    AssetOptions assets{};
    FetchOptions fetch{};
    std::optional<std::string> opl_partition;
    bool fill_missing_only{true};
};

enum class LibrarySyncGameState {
    up_to_date,   // every requested asset already present
    fetched,      // at least one asset staged for import
    nothing_found,// providers had nothing for the missing assets
    skipped,      // unreadable header, unknown startup ID or duplicate ID
};

struct LibrarySyncGame {
    std::string partition_id;
    std::string game_id;
    std::string title;
    LibrarySyncGameState state{LibrarySyncGameState::skipped};
    std::size_t already_present{};
    std::size_t missing{};
    std::size_t fetched{};
    std::vector<std::string> issues;
};

struct LibrarySyncPlan {
    bool ok{};
    bool cancelled{};
    std::string error;
    std::string opl_partition_id;
    std::uint32_t opl_partition_start_lba{};
    std::vector<LibrarySyncGame> games;
    std::vector<FetchedAsset> assets;

    [[nodiscard]] std::size_t games_with_updates() const noexcept;
};

using LibrarySyncProgress =
    std::function<void(std::size_t completed, std::size_t total, const LibrarySyncGame& game)>;

// Inventory key for an OPL asset: "<DIR>/<NAME WITHOUT EXTENSION>", upper-case.
// Classic files and TAR members of the same logical asset map to the same key.
[[nodiscard]] std::string asset_inventory_key(std::string_view target_path,
                                              std::string_view archive_member);

// Read-only inventory of ART/CFG/CHT in an OPL PFS partition, including the
// members of art.tar/cfg.tar/cht.tar when present.
[[nodiscard]] std::set<std::string> inventory_opl_assets(BlockDevice& device,
                                                         const apa::Partition& partition,
                                                         std::vector<std::string>* warnings = nullptr);

[[nodiscard]] LibrarySyncPlan plan_library_sync(BlockDevice& device,
                                                const apa::ScanResult& scan,
                                                HttpClient& http,
                                                const std::filesystem::path& cache_root,
                                                const LibrarySyncOptions& options = {},
                                                LibrarySyncProgress progress = {},
                                                std::stop_token stop = {});

struct LibrarySyncResult {
    bool ok{};
    std::string error;
    std::size_t files_written{};
    PfsImportResult import;
};

[[nodiscard]] LibrarySyncResult apply_library_sync(WritableBlockDevice& device,
                                                   const LibrarySyncPlan& plan,
                                                   const PfsImportOptions& options = {});

[[nodiscard]] const char* library_sync_state_name(LibrarySyncGameState state) noexcept;

} // namespace ps2hdd::opl
