#pragma once

#include "ps2hdd/fhdb_rescue_capture.hpp"
#include "ps2hdd/http_client.hpp"
#include "ps2hdd/magicgate_service.hpp"
#include "ps2hdd/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps2hdd::bootstrap {

enum class ProductFamily {
    fhdb,
    hdd_osd,
    hosd,
    psbbn,
    other,
};

enum class MagicGatePolicy {
    none,
    inspect_kelf,
    verify_kelf,
    sign_plaintext,
};

struct ProductStrategy {
    ProductFamily family{ProductFamily::other};
    std::string_view name{"Other"};
    MagicGatePolicy default_magicgate_policy{MagicGatePolicy::none};
    bool requires_kelf_payload{};
    bool may_require_product_filesystem_stage{};
};

[[nodiscard]] constexpr ProductStrategy strategy_for(ProductFamily family) noexcept
{
    switch (family) {
    case ProductFamily::fhdb:
        return {family, "FHDB", MagicGatePolicy::verify_kelf, true, false};
    case ProductFamily::hdd_osd:
        return {family, "HDD-OSD", MagicGatePolicy::verify_kelf, true, true};
    case ProductFamily::hosd:
        return {family, "HOSD", MagicGatePolicy::verify_kelf, true, true};
    case ProductFamily::psbbn:
        return {family, "PSBBN", MagicGatePolicy::verify_kelf, true, true};
    case ProductFamily::other:
    default:
        return {ProductFamily::other, "Other", MagicGatePolicy::none, false, false};
    }
}

struct ProviderArtifact {
    std::string provider_id;
    ProductFamily family{ProductFamily::other};
    std::string immutable_version;
    std::string asset_url;
    std::string asset_name;
    std::string expected_sha256;
    std::string provenance;
    std::size_t max_download_bytes{16U * 1024U * 1024U};
    MagicGatePolicy magicgate_policy{MagicGatePolicy::verify_kelf};
};

struct AcquiredArtifact {
    bool ok{};
    std::string error;
    ProviderArtifact manifest;
    std::vector<std::byte> bytes;
    std::string sha256;
    std::string content_type;
};

struct PreparedBootstrap {
    bool ok{};
    std::string error;
    ProviderArtifact manifest;
    std::string source_sha256;
    std::vector<std::byte> payload;
    std::string payload_sha256;
    bool magicgate_inspected{};
    bool magicgate_verified{};
    bool magicgate_signed{};
};

struct FrozenInstallInput {
    bool ok{};
    std::string error;
    ProductFamily family{ProductFamily::other};
    std::string provider_id;
    std::string immutable_version;
    std::string provenance;
    std::string source_sha256;
    std::string payload_sha256;
    std::vector<std::byte> payload;
    std::uint32_t program_start_sector{fhdb::kBootstrapProgramStartSector};
    std::uint32_t payload_sector_count{};
    std::string target_fingerprint;
};

namespace detail {

[[nodiscard]] inline std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[nodiscard]] inline bool looks_like_html(std::string_view content_type,
                                          std::span<const std::byte> body)
{
    auto type = lower_ascii(std::string(content_type));
    if (type.find("text/html") != std::string::npos ||
        type.find("application/xhtml") != std::string::npos) {
        return true;
    }
    std::string prefix;
    const auto count = std::min<std::size_t>(body.size(), 128U);
    prefix.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        prefix.push_back(static_cast<char>(std::to_integer<unsigned char>(body[i])));
    }
    prefix = lower_ascii(std::move(prefix));
    return prefix.find("<!doctype html") != std::string::npos ||
           prefix.find("<html") != std::string::npos;
}

[[nodiscard]] inline bool valid_sha256_hex(std::string_view value)
{
    if (value.size() != 64U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

[[nodiscard]] inline bool secure_url(std::string_view value)
{
    return value.size() > 8U && value.substr(0, 8U) == "https://";
}

[[nodiscard]] constexpr bool product_policy_valid(ProductFamily family,
                                                  MagicGatePolicy policy) noexcept
{
    const auto strategy = strategy_for(family);
    if (!strategy.requires_kelf_payload) return true;
    return policy == MagicGatePolicy::inspect_kelf ||
           policy == MagicGatePolicy::verify_kelf ||
           policy == MagicGatePolicy::sign_plaintext;
}

} // namespace detail

// Acquire exactly one already-resolved provider artifact. Discovery of "latest"
// may happen before this call, but this boundary requires an immutable version
// plus expected digest. A raw disk should never discover that upstream changed
// its mind after a write lease has opened.
[[nodiscard]] inline AcquiredArtifact acquire(HttpClient& http,
                                              const ProviderArtifact& manifest)
{
    AcquiredArtifact result;
    result.manifest = manifest;
    if (manifest.provider_id.empty() || manifest.immutable_version.empty() ||
        manifest.asset_name.empty() || manifest.provenance.empty()) {
        result.error = "Bootstrap provider manifest is missing immutable identity/provenance fields";
        return result;
    }
    if (!detail::secure_url(manifest.asset_url)) {
        result.error = "Bootstrap provider acquisition requires an HTTPS asset URL";
        return result;
    }
    if (!detail::valid_sha256_hex(manifest.expected_sha256)) {
        result.error = "Bootstrap provider manifest requires an exact 64-hex SHA-256";
        return result;
    }
    if (manifest.max_download_bytes == 0U ||
        manifest.max_download_bytes > 256U * 1024U * 1024U) {
        result.error = "Bootstrap provider download bound is invalid";
        return result;
    }
    if (!detail::product_policy_valid(manifest.family, manifest.magicgate_policy)) {
        result.error = "Named PS2 bootstrap families require an explicit KELF inspect/verify/sign policy";
        return result;
    }

    auto response = http.get(manifest.asset_url, manifest.max_download_bytes);
    if (!response.ok()) {
        result.error = response.error.empty()
            ? "Bootstrap provider returned HTTP status " + std::to_string(response.status)
            : response.error;
        return result;
    }
    if (response.body.empty()) {
        result.error = "Bootstrap provider returned an empty artifact";
        return result;
    }
    if (response.body.size() > manifest.max_download_bytes) {
        result.error = "Bootstrap provider exceeded the frozen download limit";
        return result;
    }
    if (detail::looks_like_html(response.content_type, response.body)) {
        result.error = "Bootstrap provider returned HTML instead of the expected binary artifact";
        return result;
    }

    result.sha256 = crypto::sha256_hex(crypto::sha256(response.body));
    if (detail::lower_ascii(result.sha256) != detail::lower_ascii(manifest.expected_sha256)) {
        result.error = "Bootstrap provider SHA-256 does not match the immutable manifest";
        return result;
    }

    result.content_type = std::move(response.content_type);
    result.bytes = std::move(response.body);
    result.ok = true;
    return result;
}

// Transform staged bytes through one explicit MagicGate policy. No HTTP client
// and no block device enter this function. The provider can therefore be tested
// against frozen fixtures and the installer can consume only already-vetted
// output rather than an exciting mixture of networking and sector writes.
[[nodiscard]] inline PreparedBootstrap prepare(
    const AcquiredArtifact& acquired,
    magicgate::MagicGateHostService* magicgate_service = nullptr,
    const std::optional<magicgate::DiskKelfSignPlan>& sign_plan = std::nullopt)
{
    PreparedBootstrap result;
    result.manifest = acquired.manifest;
    result.source_sha256 = acquired.sha256;
    if (!acquired.ok) {
        result.error = acquired.error.empty() ? "Bootstrap artifact was not acquired successfully" : acquired.error;
        return result;
    }
    if (!detail::product_policy_valid(acquired.manifest.family,
                                      acquired.manifest.magicgate_policy)) {
        result.error = "Bootstrap product strategy refuses a non-KELF policy for this family";
        return result;
    }

    switch (acquired.manifest.magicgate_policy) {
    case MagicGatePolicy::none:
        result.payload = acquired.bytes;
        break;
    case MagicGatePolicy::inspect_kelf: {
        if (!magicgate_service) {
            result.error = "Bootstrap KELF inspection requires a MagicGate host service";
            return result;
        }
        const auto inspected = magicgate_service->inspect(acquired.bytes);
        if (!inspected.ok) {
            result.error = inspected.error;
            return result;
        }
        result.magicgate_inspected = true;
        result.payload = acquired.bytes;
        break;
    }
    case MagicGatePolicy::verify_kelf: {
        if (!magicgate_service) {
            result.error = "Bootstrap KELF verification requires a MagicGate host service";
            return result;
        }
        const auto verified = magicgate_service->verify(acquired.bytes);
        if (!verified.ok) {
            result.error = verified.error;
            return result;
        }
        result.magicgate_inspected = true;
        result.magicgate_verified = true;
        result.payload = acquired.bytes;
        break;
    }
    case MagicGatePolicy::sign_plaintext: {
        if (!magicgate_service || !sign_plan) {
            result.error = "Bootstrap plaintext signing requires a MagicGate service and frozen sign plan";
            return result;
        }
        const auto signed_result = magicgate_service->sign(*sign_plan, acquired.bytes);
        if (!signed_result.ok) {
            result.error = signed_result.error;
            return result;
        }
        result.magicgate_inspected = true;
        result.magicgate_verified = true;
        result.magicgate_signed = true;
        result.payload = signed_result.file;
        break;
    }
    }

    if (result.payload.empty()) {
        result.error = "Bootstrap transformation produced an empty payload";
        return result;
    }
    if (result.payload.size() > fhdb::kBootstrapPayloadMaxBytes) {
        result.error = "Prepared bootstrap exceeds the reserved __mbr payload limit";
        result.payload.clear();
        return result;
    }
    result.payload_sha256 = crypto::sha256_hex(crypto::sha256(result.payload));
    result.ok = true;
    return result;
}

// Freeze all bytes and identity evidence before the writable target capability
// exists. `target_fingerprint` comes from the earlier read-only physical
// preflight. A caller that later sees different media must discard this object
// and plan again rather than quietly updating one field in place.
[[nodiscard]] inline FrozenInstallInput freeze_install_input(
    const PreparedBootstrap& prepared,
    std::string target_fingerprint)
{
    FrozenInstallInput result;
    if (!prepared.ok) {
        result.error = prepared.error.empty() ? "Bootstrap input is not prepared" : prepared.error;
        return result;
    }
    if (target_fingerprint.empty()) {
        result.error = "Bootstrap install input requires a frozen target fingerprint";
        return result;
    }
    if (prepared.payload.size() > fhdb::kBootstrapPayloadMaxBytes) {
        result.error = "Bootstrap payload exceeds the reserved __mbr program area";
        return result;
    }

    const auto sectors64 = (prepared.payload.size() + 511U) / 512U;
    if (sectors64 == 0U || sectors64 > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "Bootstrap payload sector geometry is invalid";
        return result;
    }

    result.family = prepared.manifest.family;
    result.provider_id = prepared.manifest.provider_id;
    result.immutable_version = prepared.manifest.immutable_version;
    result.provenance = prepared.manifest.provenance;
    result.source_sha256 = prepared.source_sha256;
    result.payload_sha256 = prepared.payload_sha256;
    result.payload = prepared.payload;
    result.payload_sector_count = static_cast<std::uint32_t>(sectors64);
    result.target_fingerprint = std::move(target_fingerprint);
    result.ok = true;
    return result;
}

} // namespace ps2hdd::bootstrap
