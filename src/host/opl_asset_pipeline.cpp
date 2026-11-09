#include "ps2hdd/opl_asset_pipeline.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace ps2hdd::opl {
namespace {

struct Downloaded {
    const AssetCandidate* candidate{};
    HttpResponse response;
};

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string bytes_to_string(const std::vector<std::byte>& bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::vector<std::byte> string_to_bytes(std::string_view text)
{
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

bool looks_like_html(const HttpResponse& response)
{
    const std::string type = lower_ascii(response.content_type);
    if (type.find("text/html") != std::string::npos) {
        return true;
    }
    if (response.body.empty()) {
        return false;
    }
    const std::size_t inspect = std::min<std::size_t>(response.body.size(), 96);
    std::string prefix(reinterpret_cast<const char*>(response.body.data()), inspect);
    prefix = lower_ascii(std::move(prefix));
    return prefix.find("<!doctype html") != std::string::npos ||
           prefix.find("<html") != std::string::npos;
}

bool safe_relative_path(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

std::filesystem::path staged_path_for(const std::filesystem::path& root,
                                      std::string_view game_id,
                                      const AssetCandidate& candidate,
                                      Placement placement)
{
    const std::filesystem::path target(candidate.target_path);
    if (placement == Placement::cache_only) {
        return root / "cache" / target;
    }
    if (!candidate.archive_member.empty()) {
        return root / "staged" / std::string(game_id) / "members" /
               target.parent_path() / candidate.archive_member;
    }
    return root / "staged" / std::string(game_id) / target;
}

bool write_file(const std::filesystem::path& path, std::span<const std::byte> bytes,
                std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Could not create asset staging directory: " + ec.message();
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Could not open staged asset file for writing";
        return false;
    }
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    output.flush();
    if (!output) {
        error = "Could not durably stage downloaded asset file";
        return false;
    }
    return true;
}

std::optional<std::size_t> existing_nonempty_size(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(size);
}

Downloaded download(HttpClient& http, const AssetCandidate& candidate, std::size_t max_bytes)
{
    Downloaded result;
    result.candidate = &candidate;
    result.response = http.get(candidate.url, max_bytes);
    if (result.response.ok() && result.response.body.empty()) {
        result.response.error = "provider returned an empty response";
    }
    if (result.response.ok() && looks_like_html(result.response)) {
        result.response.error = "provider returned HTML instead of the requested asset";
    }
    return result;
}

std::string describe_failure(const Downloaded& item)
{
    if (!item.response.error.empty()) {
        return item.candidate->provider + ": " + item.response.error;
    }
    return item.candidate->provider + ": HTTP " + std::to_string(item.response.status);
}

void add_issue(FetchResult& result, AssetKind kind, bool fatal, std::string message)
{
    result.issues.push_back({kind, fatal, std::move(message)});
    if (fatal) {
        result.ok = false;
    }
}

bool emit_asset(FetchResult& result,
                const AssetPlan& plan,
                const AssetRequest& request,
                const AssetCandidate& candidate,
                std::span<const std::byte> bytes,
                const std::filesystem::path& cache_root,
                std::string provider,
                std::string source_url,
                bool merged,
                bool verified_mastercode,
                bool from_cache = false)
{
    const std::filesystem::path relative_target(candidate.target_path);
    const std::filesystem::path archive_member(candidate.archive_member);
    if (!safe_relative_path(relative_target) ||
        (!candidate.archive_member.empty() && !safe_relative_path(archive_member))) {
        add_issue(result, request.kind, true, "Asset provider generated an unsafe staging path");
        return false;
    }

    const auto path = staged_path_for(cache_root, plan.game_id, candidate, request.placement);
    if (!from_cache) {
        std::string error;
        if (!write_file(path, bytes, error)) {
            add_issue(result, request.kind, true, std::move(error));
            return false;
        }
    }

    FetchedAsset asset;
    asset.kind = request.kind;
    asset.placement = request.placement;
    asset.provider = std::move(provider);
    asset.source_url = std::move(source_url);
    asset.target_path = candidate.target_path;
    asset.archive_member = candidate.archive_member;
    asset.staged_path = path;
    asset.bytes = bytes.size();
    if (from_cache) {
        if (const auto size = existing_nonempty_size(path)) {
            asset.bytes = *size;
        }
    }
    asset.merged = merged;
    asset.verified_mastercode = verified_mastercode;
    asset.from_cache = from_cache;
    result.assets.push_back(std::move(asset));
    return true;
}

void fetch_first_available(FetchResult& result,
                           const AssetPlan& plan,
                           const AssetRequest& request,
                           HttpClient& http,
                           const std::filesystem::path& cache_root,
                           const FetchOptions& options)
{
    std::string last_error = "no provider candidates";
    for (const auto& candidate : request.candidates) {
        const auto path = staged_path_for(cache_root, plan.game_id, candidate, request.placement);
        if (request.placement == Placement::cache_only && !options.refresh_catalog) {
            if (existing_nonempty_size(path)) {
                emit_asset(result, plan, request, candidate, {}, cache_root,
                           candidate.provider, candidate.url, false, false, true);
                return;
            }
        }

        auto item = download(http, candidate, options.max_download_bytes);
        if (!item.response.ok()) {
            last_error = describe_failure(item);
            continue;
        }
        emit_asset(result, plan, request, candidate, item.response.body, cache_root,
                   candidate.provider, candidate.url, false, false);
        return;
    }
    add_issue(result, request.kind, !request.optional,
              std::string(asset_kind_name(request.kind)) + " unavailable; " + last_error);
}

void fetch_cfg(FetchResult& result,
               const AssetPlan& plan,
               const AssetRequest& request,
               HttpClient& http,
               const std::filesystem::path& cache_root,
               const FetchOptions& options)
{
    std::vector<Downloaded> successful;
    std::string failures;
    for (const auto& candidate : request.candidates) {
        auto item = download(http, candidate, options.max_download_bytes);
        if (item.response.ok()) {
            successful.push_back(std::move(item));
        } else {
            if (!failures.empty()) {
                failures += "; ";
            }
            failures += describe_failure(item);
        }
    }

    if (successful.empty()) {
        add_issue(result, request.kind, !request.optional,
                  "OPL CFG unavailable" + (failures.empty() ? std::string{} : "; " + failures));
        return;
    }

    const auto* target = successful.front().candidate;
    std::string content = bytes_to_string(successful.front().response.body);
    std::string provider = successful.front().candidate->provider;
    std::string source_url = successful.front().candidate->url;
    bool merged = false;
    if (successful.size() >= 2) {
        content = merge_cfg_overlay(content, bytes_to_string(successful[1].response.body));
        provider += " + " + successful[1].candidate->provider;
        source_url += " + " + successful[1].candidate->url;
        merged = true;
    }

    const auto bytes = string_to_bytes(content);
    emit_asset(result, plan, request, *target, bytes, cache_root,
               std::move(provider), std::move(source_url), merged, false);
}

void fetch_widescreen(FetchResult& result,
                      const AssetPlan& plan,
                      const AssetRequest& request,
                      HttpClient& http,
                      const std::filesystem::path& cache_root,
                      const FetchOptions& options)
{
    if (request.candidates.empty()) {
        add_issue(result, request.kind, !request.optional, "widescreen request has no provider");
        return;
    }

    auto primary = download(http, request.candidates.front(), options.max_download_bytes);
    if (!primary.response.ok()) {
        add_issue(result, request.kind, !request.optional,
                  "widescreen CHT unavailable; " + describe_failure(primary));
        return;
    }

    std::string content = bytes_to_string(primary.response.body);
    bool verified = cht_has_mastercode(content);
    bool merged = false;
    std::string provider = primary.candidate->provider;
    std::string source_url = primary.candidate->url;

    if (!verified && request.merge_policy == MergePolicy::merge_mastercode_if_needed &&
        request.candidates.size() >= 2) {
        auto fallback = download(http, request.candidates[1], options.max_download_bytes);
        if (fallback.response.ok()) {
            const std::string master = bytes_to_string(fallback.response.body);
            if (cht_has_mastercode(master)) {
                content = merge_cht_with_mastercode(content, master);
                verified = cht_has_mastercode(content);
                merged = verified;
                provider += " + " + fallback.candidate->provider;
                source_url += " + " + fallback.candidate->url;
            } else {
                add_issue(result, request.kind, false,
                          "bare mastercode provider returned a CHT without a Mastercode section");
            }
        } else {
            add_issue(result, request.kind, false,
                      "widescreen fix found but verified mastercode is unavailable; " +
                          describe_failure(fallback));
        }
    }

    const auto bytes = string_to_bytes(content);
    emit_asset(result, plan, request, *primary.candidate, bytes, cache_root,
               std::move(provider), std::move(source_url), merged, verified);
}

} // namespace

FetchResult fetch_assets(const AssetPlan& plan,
                         HttpClient& http,
                         const std::filesystem::path& cache_root,
                         const FetchOptions& options)
{
    FetchResult result;
    result.ok = true;
    if (!plan.ok) {
        result.ok = false;
        result.issues.push_back({AssetKind::title_database, true,
                                 "Cannot fetch assets from an invalid plan: " + plan.error});
        return result;
    }
    if (cache_root.empty()) {
        result.ok = false;
        result.issues.push_back({AssetKind::title_database, true,
                                 "Asset cache root is empty"});
        return result;
    }

    for (const auto& request : plan.requests) {
        switch (request.merge_policy) {
        case MergePolicy::first_available:
            fetch_first_available(result, plan, request, http, cache_root, options);
            break;
        case MergePolicy::merge_cfg_overlay:
            fetch_cfg(result, plan, request, http, cache_root, options);
            break;
        case MergePolicy::merge_mastercode_if_needed:
            fetch_widescreen(result, plan, request, http, cache_root, options);
            break;
        }
    }

    return result;
}

} // namespace ps2hdd::opl
