#pragma once

#include "ps2hdd/http_client.hpp"
#include "ps2hdd/opl_assets.hpp"

#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace ps2hdd::opl {

struct FetchOptions {
    std::size_t max_download_bytes{16U * 1024U * 1024U};
    bool refresh_catalog{};
};

struct FetchedAsset {
    AssetKind kind{};
    Placement placement{};
    std::string provider;
    std::string source_url;
    std::string target_path;
    std::string archive_member;
    std::filesystem::path staged_path;
    std::size_t bytes{};
    bool merged{};
    bool verified_mastercode{};
    bool from_cache{};
};

struct FetchIssue {
    AssetKind kind{};
    bool fatal{};
    std::string message;
};

struct FetchResult {
    bool ok{};
    std::vector<FetchedAsset> assets;
    std::vector<FetchIssue> issues;
};

// Downloads into a host-side staging/cache tree only. Nothing in this layer is
// allowed to write PFS or physical disks. A later PFS transaction consumes the
// verified staged files after the user has seen the complete install preview.
[[nodiscard]] FetchResult fetch_assets(const AssetPlan& plan,
                                       HttpClient& http,
                                       const std::filesystem::path& cache_root,
                                       const FetchOptions& options = {});

} // namespace ps2hdd::opl
