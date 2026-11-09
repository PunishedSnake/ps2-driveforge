#pragma once

#include "ps2hdd/bootstrap_provider.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace ps2hdd::bootstrap {

struct ManifestParseResult {
    bool ok{};
    std::size_t line{};
    std::string error;
    ProviderArtifact artifact;
};

namespace manifest_detail {

[[nodiscard]] constexpr bool whitespace(char ch) noexcept
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
    while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
    return text;
}

[[nodiscard]] inline bool parse_family(std::string_view value, ProductFamily& out) noexcept
{
    if (value == "fhdb") out = ProductFamily::fhdb;
    else if (value == "hdd-osd" || value == "hdd_osd") out = ProductFamily::hdd_osd;
    else if (value == "hosd") out = ProductFamily::hosd;
    else if (value == "psbbn") out = ProductFamily::psbbn;
    else if (value == "other") out = ProductFamily::other;
    else return false;
    return true;
}

[[nodiscard]] inline bool parse_policy(std::string_view value, MagicGatePolicy& out) noexcept
{
    if (value == "none") out = MagicGatePolicy::none;
    else if (value == "inspect") out = MagicGatePolicy::inspect_kelf;
    else if (value == "verify") out = MagicGatePolicy::verify_kelf;
    else if (value == "sign") out = MagicGatePolicy::sign_plaintext;
    else return false;
    return true;
}

[[nodiscard]] inline bool parse_size(std::string_view value, std::size_t& out) noexcept
{
    if (value.empty()) return false;
    std::size_t parsed{};
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto conversion = std::from_chars(begin, end, parsed, 10);
    if (conversion.ec != std::errc{} || conversion.ptr != end) return false;
    out = parsed;
    return true;
}

} // namespace manifest_detail

// Strict key=value manifest intended for local/user-controlled provider files.
// Unknown keys, duplicates and incomplete records are rejected rather than
// silently ignored. That makes an old manifest fail loudly when the contract
// changes instead of acquiring a different payload with defaulted semantics.
//
// Required fields:
//   provider_id
//   family = fhdb | hdd-osd | hosd | psbbn | other
//   version
//   asset_url
//   asset_name
//   sha256
//   provenance
//   magicgate = none | inspect | verify | sign
// Optional:
//   max_download_bytes (defaults to ProviderArtifact's bounded default)
[[nodiscard]] inline ManifestParseResult parse_provider_manifest(std::string_view text)
{
    ManifestParseResult result;
    if (text.empty()) {
        result.error = "Bootstrap provider manifest is empty";
        return result;
    }
    if (text.size() > 128U * 1024U) {
        result.error = "Bootstrap provider manifest exceeds the 128 KiB parser limit";
        return result;
    }

    enum Field : std::size_t {
        provider_id,
        family,
        version,
        asset_url,
        asset_name,
        sha256,
        provenance,
        magicgate,
        max_download_bytes,
        field_count,
    };
    std::array<bool, field_count> seen{};

    const auto duplicate = [&](Field field) {
        if (seen[field]) {
            result.error = "Duplicate bootstrap provider manifest field";
            return true;
        }
        seen[field] = true;
        return false;
    };

    std::size_t cursor = 0;
    std::size_t line_number = 0;
    while (cursor <= text.size()) {
        ++line_number;
        const auto newline = text.find('\n', cursor);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        auto line = manifest_detail::trim(text.substr(cursor, end - cursor));
        cursor = newline == std::string_view::npos ? text.size() + 1U : newline + 1U;
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;

        const auto equals = line.find('=');
        if (equals == std::string_view::npos || line.find('=', equals + 1U) != std::string_view::npos) {
            result.line = line_number;
            result.error = "Bootstrap provider manifest line must contain exactly one '='";
            return result;
        }
        const auto key = manifest_detail::trim(line.substr(0, equals));
        const auto value = manifest_detail::trim(line.substr(equals + 1U));
        if (key.empty() || value.empty()) {
            result.line = line_number;
            result.error = "Bootstrap provider manifest key/value must be non-empty";
            return result;
        }

        bool assigned = true;
        if (key == "provider_id") {
            if (duplicate(provider_id)) { result.line = line_number; return result; }
            result.artifact.provider_id = std::string(value);
        } else if (key == "family") {
            if (duplicate(family)) { result.line = line_number; return result; }
            if (!manifest_detail::parse_family(value, result.artifact.family)) assigned = false;
        } else if (key == "version") {
            if (duplicate(version)) { result.line = line_number; return result; }
            result.artifact.immutable_version = std::string(value);
        } else if (key == "asset_url") {
            if (duplicate(asset_url)) { result.line = line_number; return result; }
            result.artifact.asset_url = std::string(value);
        } else if (key == "asset_name") {
            if (duplicate(asset_name)) { result.line = line_number; return result; }
            result.artifact.asset_name = std::string(value);
        } else if (key == "sha256") {
            if (duplicate(sha256)) { result.line = line_number; return result; }
            result.artifact.expected_sha256 = std::string(value);
        } else if (key == "provenance") {
            if (duplicate(provenance)) { result.line = line_number; return result; }
            result.artifact.provenance = std::string(value);
        } else if (key == "magicgate") {
            if (duplicate(magicgate)) { result.line = line_number; return result; }
            if (!manifest_detail::parse_policy(value, result.artifact.magicgate_policy)) assigned = false;
        } else if (key == "max_download_bytes") {
            if (duplicate(max_download_bytes)) { result.line = line_number; return result; }
            if (!manifest_detail::parse_size(value, result.artifact.max_download_bytes)) assigned = false;
        } else {
            result.line = line_number;
            result.error = "Unknown bootstrap provider manifest field: " + std::string(key);
            return result;
        }

        if (!assigned) {
            result.line = line_number;
            result.error = "Invalid value for bootstrap provider manifest field: " + std::string(key);
            return result;
        }
    }

    constexpr std::array<Field, 8> required{
        provider_id, family, version, asset_url, asset_name, sha256, provenance, magicgate};
    for (const auto field : required) {
        if (!seen[field]) {
            result.error = "Bootstrap provider manifest is missing one or more required fields";
            return result;
        }
    }
    if (!detail::product_policy_valid(result.artifact.family, result.artifact.magicgate_policy)) {
        result.error = "Bootstrap provider manifest selects an invalid MagicGate policy for the product family";
        return result;
    }
    if (!detail::secure_url(result.artifact.asset_url)) {
        result.error = "Bootstrap provider manifest asset_url must use HTTPS";
        return result;
    }
    if (!detail::valid_sha256_hex(result.artifact.expected_sha256)) {
        result.error = "Bootstrap provider manifest sha256 must contain exactly 64 hexadecimal characters";
        return result;
    }
    if (result.artifact.max_download_bytes == 0U ||
        result.artifact.max_download_bytes > 256U * 1024U * 1024U) {
        result.error = "Bootstrap provider manifest max_download_bytes is outside the accepted range";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::bootstrap
