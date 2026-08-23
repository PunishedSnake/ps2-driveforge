#pragma once

#include "ps2hdd/bootstrap_manifest.hpp"
#include "ps2hdd/bootstrap_provider.hpp"
#include "ps2hdd/http_client.hpp"
#include "ps2hdd/magicgate_service.hpp"
#include "ps2hdd/physical_bootstrap_recovery.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/sha256.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ps2df::winui {

struct BootstrapProviderPreview {
    bool ok{};
    std::string error;
    std::string product;
    std::string provider_id;
    std::string version;
    std::string provenance;
    std::string source_sha256;
    std::string payload_sha256;
    std::uint64_t payload_bytes{};
    std::uint32_t payload_sectors{};
    std::string target_fingerprint;
    ps2hdd::bootstrap::FrozenInstallInput frozen;
};

struct BootstrapProviderInstallSnapshot {
    bool ok{};
    bool partial{};
    std::string error;
    std::filesystem::path rescue_path;
    std::filesystem::path hddmbr_path;
    bool cold_master_verified{};
    bool cold_payload_verified{};
};

namespace provider_detail {

[[nodiscard]] inline bool read_text_bounded(const std::filesystem::path& path,
                                            std::size_t max_bytes,
                                            std::string& text,
                                            std::string& error)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Could not open local file: " + path.string();
        return false;
    }
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_bytes) {
        error = "Local file exceeds its bounded read limit: " + path.string();
        return false;
    }
    text.resize(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        error = "Could not read local file completely: " + path.string();
        return false;
    }
    return true;
}

[[nodiscard]] inline int hex_nibble(wchar_t ch) noexcept
{
    if (ch >= L'0' && ch <= L'9') return static_cast<int>(ch - L'0');
    if (ch >= L'a' && ch <= L'f') return static_cast<int>(ch - L'a') + 10;
    if (ch >= L'A' && ch <= L'F') return static_cast<int>(ch - L'A') + 10;
    return -1;
}

[[nodiscard]] inline bool parse_icvps2(std::wstring_view text,
                                      ps2hdd::magicgate::cipher::Block& out,
                                      std::string& error)
{
    if (text.empty()) return false;
    if (text.size() != out.size() * 2U) {
        error = "ICVPS2 must contain exactly 16 hexadecimal characters (8 bytes)";
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = hex_nibble(text[i * 2U]);
        const int lo = hex_nibble(text[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            error = "ICVPS2 contains a non-hexadecimal character";
            return false;
        }
        out[i] = static_cast<std::byte>((hi << 4) | lo);
    }
    return true;
}

} // namespace provider_detail

// Complete every network/crypto/planning step while the target is still opened
// read-only. The returned FrozenInstallInput is the only object allowed to cross
// into the guarded physical installer. This makes the GUI preview a real safety
// boundary rather than a decorative progress page.
[[nodiscard]] inline BootstrapProviderPreview stage_bootstrap_provider(
    unsigned physical_index,
    const std::filesystem::path& manifest_path,
    const std::filesystem::path& keyset_path,
    std::wstring_view icvps2_hex,
    std::string icvps2_provenance)
{
    BootstrapProviderPreview result;
    std::string manifest_text;
    if (!provider_detail::read_text_bounded(
            manifest_path, 128U * 1024U, manifest_text, result.error)) {
        return result;
    }
    const auto parsed = ps2hdd::bootstrap::parse_provider_manifest(manifest_text);
    if (!parsed.ok) {
        result.error = parsed.error;
        if (parsed.line != 0U) result.error += " at line " + std::to_string(parsed.line);
        return result;
    }

    ps2hdd::PhysicalDrive disk(physical_index);
    if (!disk.is_open()) {
        result.error = "Could not reopen PhysicalDrive" + std::to_string(physical_index) + " read-only for provider staging";
        return result;
    }
    const auto admission = ps2hdd::physical_write::authorize(disk);
    if (!admission.ok) {
        result.error = "Physical provider preflight refused the target: " + admission.error;
        return result;
    }
    result.target_fingerprint = ps2hdd::crypto::sha256_hex(admission.authorization.identity.digest);

    auto http = ps2hdd::make_platform_http_client();
    if (!http) {
        result.error = "No native HTTPS provider transport is available on this platform";
        return result;
    }
    const auto acquired = ps2hdd::bootstrap::acquire(*http, parsed.artifact);
    if (!acquired.ok) {
        result.error = acquired.error;
        return result;
    }

    ps2hdd::magicgate::MagicGateHostService magicgate;
    if (parsed.artifact.magicgate_policy == ps2hdd::bootstrap::MagicGatePolicy::verify_kelf ||
        parsed.artifact.magicgate_policy == ps2hdd::bootstrap::MagicGatePolicy::sign_plaintext) {
        std::string keyset_text;
        if (keyset_path.empty()) {
            result.error = "This provider policy requires a local MagicGate keyset file";
            return result;
        }
        if (!provider_detail::read_text_bounded(
                keyset_path, magicgate.limits().max_keyset_bytes, keyset_text, result.error)) {
            return result;
        }
        std::string key_error;
        if (!magicgate.load_keyset_text(
                keyset_text, "local file: " + keyset_path.string(), key_error)) {
            result.error = key_error;
            return result;
        }
    }

    if (!icvps2_hex.empty()) {
        ps2hdd::magicgate::cipher::Block icv{};
        if (!provider_detail::parse_icvps2(icvps2_hex, icv, result.error)) return result;
        if (icvps2_provenance.empty()) {
            result.error = "ICVPS2 evidence requires an explicit provenance label";
            return result;
        }
        std::string evidence_error;
        if (!magicgate.load_icvps2(icv, std::move(icvps2_provenance), evidence_error)) {
            result.error = evidence_error;
            return result;
        }
    }

    if (parsed.artifact.magicgate_policy == ps2hdd::bootstrap::MagicGatePolicy::sign_plaintext) {
        // Signing itself is implemented and tested, but a provider sign plan is
        // format input, not something the GUI may infer from a filename. Keep
        // this path fail-closed until a manifest explicitly serializes the plan.
        result.error = "Provider manifest requests MagicGate signing, but no serialized sign plan is present; use an already signed/verified KELF or a typed provider strategy";
        return result;
    }

    const auto prepared = ps2hdd::bootstrap::prepare(acquired, &magicgate);
    if (!prepared.ok) {
        result.error = prepared.error;
        return result;
    }
    result.frozen = ps2hdd::bootstrap::freeze_install_input(
        prepared, result.target_fingerprint);
    if (!result.frozen.ok) {
        result.error = result.frozen.error;
        return result;
    }
    std::string frozen_error;
    if (!ps2hdd::bootstrap::validate_frozen_payload(result.frozen, frozen_error)) {
        result.error = frozen_error;
        result.frozen = {};
        return result;
    }

    const auto strategy = ps2hdd::bootstrap::strategy_for(parsed.artifact.family);
    result.product = std::string(strategy.name);
    result.provider_id = parsed.artifact.provider_id;
    result.version = parsed.artifact.immutable_version;
    result.provenance = parsed.artifact.provenance;
    result.source_sha256 = prepared.source_sha256;
    result.payload_sha256 = prepared.payload_sha256;
    result.payload_bytes = prepared.payload.size();
    result.payload_sectors = result.frozen.payload_sector_count;
    result.ok = true;
    return result;
}

[[nodiscard]] inline BootstrapProviderInstallSnapshot commit_bootstrap_provider(
    unsigned physical_index,
    const ps2hdd::bootstrap::FrozenInstallInput& frozen,
    const std::filesystem::path& safety_directory)
{
    BootstrapProviderInstallSnapshot result;
    std::string frozen_error;
    if (!ps2hdd::bootstrap::validate_frozen_payload(frozen, frozen_error)) {
        result.error = frozen_error;
        return result;
    }
    ps2hdd::PhysicalBootstrapInstallOptions options;
    options.safety_directory = safety_directory;
    const auto installed = ps2hdd::install_bootstrap_to_physical(
        physical_index, frozen, options);
    result.ok = installed.ok;
    result.partial = installed.partial;
    result.error = installed.error;
    result.rescue_path = installed.rescue_capsule_path;
    result.hddmbr_path = installed.hddmbr_path;
    result.cold_master_verified = installed.cold_master_verified;
    result.cold_payload_verified = installed.cold_payload_verified;
    return result;
}

} // namespace ps2df::winui
