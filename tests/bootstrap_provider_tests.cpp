#include "ps2hdd/bootstrap_provider.hpp"
#include "ps2hdd/magicgate_payload_known_vectors.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class FakeHttp final : public ps2hdd::HttpClient {
public:
    ps2hdd::HttpResponse response;
    std::string requested_url;
    std::size_t requested_limit{};

    ps2hdd::HttpResponse get(std::string_view url, std::size_t max_bytes) override
    {
        requested_url = std::string(url);
        requested_limit = max_bytes;
        return response;
    }
};

ps2hdd::magicgate::DiskKelfSignPlan make_sign_plan()
{
    namespace mg = ps2hdd::magicgate;
    mg::DiskKelfSignPlan plan;
    for (std::size_t i = 0; i < plan.header.user_header.size(); ++i) {
        plan.header.user_header[i] = static_cast<std::byte>(0x41U + i * 3U);
    }
    plan.header.unknown5 = 0x0701U;
    plan.header.flags = 0x0020U;
    plan.header.mg_zones = 1U;
    plan.content_keys.kbit = mg::known_vectors::kSyntheticKbit;
    plan.content_keys.kc = mg::known_vectors::kSyntheticKc;
    plan.signed_flag = 0x02U;
    plan.encrypted_flag = 0x01U;
    plan.blocks = {{32U, 0x03U}, {32U, 0x02U}, {32U, 0x01U}};
    return plan;
}

std::vector<std::byte> canonical_kelf()
{
    namespace mg = ps2hdd::magicgate;
    const auto signed_kelf = mg::sign_low_layout_disk_kelf(
        make_sign_plan(), mg::payload_known_vectors::kPlaintextPayload,
        mg::known_vectors::kSyntheticKeyset);
    check(signed_kelf.ok, "canonical provider KELF fixture must sign");
    return signed_kelf.file;
}

ps2hdd::bootstrap::ProviderArtifact make_manifest(
    std::span<const std::byte> body,
    ps2hdd::bootstrap::MagicGatePolicy policy = ps2hdd::bootstrap::MagicGatePolicy::verify_kelf)
{
    ps2hdd::bootstrap::ProviderArtifact manifest;
    manifest.provider_id = "fixture-fhdb";
    manifest.family = ps2hdd::bootstrap::ProductFamily::fhdb;
    manifest.immutable_version = "v1.2.3";
    manifest.asset_url = "https://example.invalid/releases/v1.2.3/MBR.KELF";
    manifest.asset_name = "MBR.KELF";
    manifest.expected_sha256 = ps2hdd::crypto::sha256_hex(ps2hdd::crypto::sha256(body));
    manifest.provenance = "fixture repository / immutable v1.2.3 / MBR.KELF";
    manifest.max_download_bytes = 4U * 1024U * 1024U;
    manifest.magicgate_policy = policy;
    return manifest;
}

void test_product_strategies_are_explicit()
{
    namespace bs = ps2hdd::bootstrap;
    for (const auto family : {bs::ProductFamily::fhdb, bs::ProductFamily::hdd_osd,
                              bs::ProductFamily::hosd, bs::ProductFamily::psbbn}) {
        const auto strategy = bs::strategy_for(family);
        check(strategy.family == family, "bootstrap strategy must retain its product family");
        check(strategy.requires_kelf_payload,
              "named PS2 HDD bootstrap families must require an explicit KELF payload policy");
        check(strategy.default_magicgate_policy == bs::MagicGatePolicy::verify_kelf,
              "named product strategies must default to cryptographic KELF verification");
    }
    check(!bs::strategy_for(bs::ProductFamily::fhdb).may_require_product_filesystem_stage,
          "FHDB bootstrap strategy should not invent an unrelated product filesystem stage");
    check(bs::strategy_for(bs::ProductFamily::hdd_osd).may_require_product_filesystem_stage,
          "HDD-OSD strategy must preserve its product filesystem-stage requirement");
    check(bs::strategy_for(bs::ProductFamily::hosd).may_require_product_filesystem_stage,
          "HOSD strategy must preserve its product filesystem-stage requirement");
    check(bs::strategy_for(bs::ProductFamily::psbbn).may_require_product_filesystem_stage,
          "PSBBN strategy must preserve its product filesystem-stage requirement");
}

void test_acquire_hashes_and_freezes_identity()
{
    const auto file = canonical_kelf();
    FakeHttp http;
    http.response.status = 200;
    http.response.body = file;
    http.response.content_type = "application/octet-stream";
    const auto manifest = make_manifest(file);

    const auto acquired = ps2hdd::bootstrap::acquire(http, manifest);
    check(acquired.ok, "immutable provider artifact must acquire with matching SHA-256");
    check(http.requested_url == manifest.asset_url, "provider must request the frozen HTTPS URL");
    check(http.requested_limit == manifest.max_download_bytes, "provider must pass the frozen byte bound to transport");
    check(acquired.sha256 == manifest.expected_sha256, "provider acquisition digest mismatch");
}

void test_acquire_rejects_html_and_hash_mismatch()
{
    const auto file = canonical_kelf();
    auto manifest = make_manifest(file);

    FakeHttp html;
    html.response.status = 200;
    const std::string page = "<!doctype html><html><body>not a KELF</body></html>";
    html.response.body.assign(reinterpret_cast<const std::byte*>(page.data()),
                              reinterpret_cast<const std::byte*>(page.data() + page.size()));
    html.response.content_type = "text/html; charset=utf-8";
    check(!ps2hdd::bootstrap::acquire(html, manifest).ok,
          "provider must reject HTML masquerading as a successful binary download");

    FakeHttp changed;
    changed.response.status = 200;
    changed.response.body = file;
    changed.response.body.back() ^= std::byte{0x01};
    changed.response.content_type = "application/octet-stream";
    check(!ps2hdd::bootstrap::acquire(changed, manifest).ok,
          "provider must reject bytes whose SHA-256 differs from the frozen manifest");
}

void test_verify_policy_uses_magicgate_service()
{
    namespace mg = ps2hdd::magicgate;
    const auto file = canonical_kelf();
    FakeHttp http;
    http.response.status = 200;
    http.response.body = file;
    http.response.content_type = "application/octet-stream";
    const auto acquired = ps2hdd::bootstrap::acquire(http, make_manifest(file));
    check(acquired.ok, "provider verification fixture must acquire");

    mg::MagicGateHostService service;
    std::string error;
    check(service.load_keyset(mg::known_vectors::kSyntheticKeyset,
                              "synthetic provider test keyset", error),
          "provider test keyset must load");
    const auto prepared = ps2hdd::bootstrap::prepare(acquired, &service);
    check(prepared.ok, "verified provider KELF must prepare");
    check(prepared.magicgate_inspected && prepared.magicgate_verified,
          "verify policy must record MagicGate inspection and verification");
    check(prepared.payload == file, "verify policy must preserve the verified KELF bytes exactly");

    const auto frozen = ps2hdd::bootstrap::freeze_install_input(prepared, "disk-fingerprint-1234");
    check(frozen.ok, "prepared bootstrap must freeze against a physical target identity");
    check(frozen.target_fingerprint == "disk-fingerprint-1234",
          "frozen bootstrap must retain target identity evidence");
    check(frozen.payload_sha256 == prepared.payload_sha256,
          "frozen bootstrap must retain transformed payload SHA-256");
    check(frozen.program_start_sector == ps2hdd::fhdb::kBootstrapProgramStartSector,
          "frozen bootstrap must target the canonical __mbr program area");
    check(frozen.payload_sector_count == (frozen.payload.size() + 511U) / 512U,
          "frozen bootstrap sector geometry must exactly cover its immutable payload");
}

void test_verify_fails_without_key_capability()
{
    const auto file = canonical_kelf();
    FakeHttp http;
    http.response.status = 200;
    http.response.body = file;
    http.response.content_type = "application/octet-stream";
    const auto acquired = ps2hdd::bootstrap::acquire(http, make_manifest(file));
    ps2hdd::magicgate::MagicGateHostService service;
    check(!ps2hdd::bootstrap::prepare(acquired, &service).ok,
          "provider verify policy must fail closed without a MagicGate keyset capability");
}

void test_sign_plaintext_policy_is_reverified()
{
    namespace mg = ps2hdd::magicgate;
    const auto plaintext = std::vector<std::byte>(
        mg::payload_known_vectors::kPlaintextPayload.begin(),
        mg::payload_known_vectors::kPlaintextPayload.end());
    FakeHttp http;
    http.response.status = 200;
    http.response.body = plaintext;
    http.response.content_type = "application/octet-stream";
    const auto acquired = ps2hdd::bootstrap::acquire(
        http, make_manifest(plaintext, ps2hdd::bootstrap::MagicGatePolicy::sign_plaintext));
    check(acquired.ok, "plaintext provider artifact must acquire");

    mg::MagicGateHostService service;
    std::string error;
    check(service.load_keyset(mg::known_vectors::kSyntheticKeyset,
                              "synthetic provider signer", error),
          "provider signer keyset must load");
    const auto prepared = ps2hdd::bootstrap::prepare(acquired, &service, make_sign_plan());
    check(prepared.ok, "sign-plaintext policy must produce a verified KELF");
    check(prepared.magicgate_signed && prepared.magicgate_verified,
          "signed provider output must also be independently verified");
    check(prepared.payload != plaintext, "sign-plaintext policy must transform source bytes into a KELF");
}

void test_mutable_insecure_or_policyless_manifest_is_refused_before_network()
{
    namespace bs = ps2hdd::bootstrap;
    const auto file = canonical_kelf();

    FakeHttp missing_version;
    missing_version.response.status = 200;
    missing_version.response.body = file;
    auto manifest = make_manifest(file);
    manifest.immutable_version.clear();
    check(!bs::acquire(missing_version, manifest).ok,
          "provider must require resolved immutable version identity");
    check(missing_version.requested_url.empty(), "invalid version must fail before network acquisition");

    FakeHttp insecure;
    insecure.response.status = 200;
    insecure.response.body = file;
    manifest = make_manifest(file);
    manifest.asset_url = "http://example.invalid/MBR.KELF";
    check(!bs::acquire(insecure, manifest).ok,
          "provider must reject non-HTTPS asset URLs before transport");
    check(insecure.requested_url.empty(), "insecure URL must fail before network acquisition");

    FakeHttp policyless;
    policyless.response.status = 200;
    policyless.response.body = file;
    manifest = make_manifest(file, bs::MagicGatePolicy::none);
    check(!bs::acquire(policyless, manifest).ok,
          "named PS2 bootstrap family must not bypass KELF inspection/verification policy");
    check(policyless.requested_url.empty(), "invalid product crypto policy must fail before network acquisition");
}

} // namespace

int main()
{
    try {
        test_product_strategies_are_explicit();
        test_acquire_hashes_and_freezes_identity();
        test_acquire_rejects_html_and_hash_mismatch();
        test_verify_policy_uses_magicgate_service();
        test_verify_fails_without_key_capability();
        test_sign_plaintext_policy_is_reverified();
        test_mutable_insecure_or_policyless_manifest_is_refused_before_network();
        std::cout << "Bootstrap provider staging tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Bootstrap provider staging tests failed: " << error.what() << '\n';
        return 1;
    }
}
