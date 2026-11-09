#include "ps2hdd/bootstrap_provider.hpp"
#include "ps2hdd/magicgate_payload_known_vectors.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::byte> canonical_kelf()
{
    namespace mg = ps2hdd::magicgate;
    mg::DiskKelfSignPlan plan;
    for (std::size_t i = 0; i < plan.header.user_header.size(); ++i)
        plan.header.user_header[i] = static_cast<std::byte>(0x61U + i);
    plan.header.unknown5 = 0x0701U;
    plan.header.flags = 0x0020U;
    plan.header.mg_zones = 1U;
    plan.content_keys.kbit = mg::known_vectors::kSyntheticKbit;
    plan.content_keys.kc = mg::known_vectors::kSyntheticKc;
    plan.signed_flag = 0x02U;
    plan.encrypted_flag = 0x01U;
    plan.blocks = {{32U, 0x03U}, {32U, 0x02U}, {32U, 0x01U}};
    const auto signed_kelf = mg::sign_low_layout_disk_kelf(
        plan, mg::payload_known_vectors::kPlaintextPayload,
        mg::known_vectors::kSyntheticKeyset);
    check(signed_kelf.ok, "install-policy KELF fixture must sign");
    return signed_kelf.file;
}

void test_inspection_only_named_product_cannot_freeze_for_install()
{
    namespace bs = ps2hdd::bootstrap;
    const auto payload = canonical_kelf();
    bs::PreparedBootstrap prepared;
    prepared.ok = true;
    prepared.manifest.family = bs::ProductFamily::fhdb;
    prepared.manifest.provider_id = "inspection-only";
    prepared.manifest.immutable_version = "v1";
    prepared.manifest.provenance = "synthetic fixture";
    prepared.payload = payload;
    prepared.payload_sha256 = ps2hdd::crypto::sha256_hex(ps2hdd::crypto::sha256(payload));
    prepared.magicgate_inspected = true;
    prepared.magicgate_verified = false;

    const auto frozen = bs::freeze_install_input(prepared, "target-fingerprint");
    check(!frozen.ok,
          "structural KELF inspection alone must not create an installable FHDB frozen plan");
}

void test_verified_named_product_freezes_and_retains_evidence()
{
    namespace bs = ps2hdd::bootstrap;
    const auto payload = canonical_kelf();
    bs::PreparedBootstrap prepared;
    prepared.ok = true;
    prepared.manifest.family = bs::ProductFamily::fhdb;
    prepared.manifest.provider_id = "verified";
    prepared.manifest.immutable_version = "v1";
    prepared.manifest.provenance = "synthetic fixture";
    prepared.payload = payload;
    prepared.payload_sha256 = ps2hdd::crypto::sha256_hex(ps2hdd::crypto::sha256(payload));
    prepared.magicgate_inspected = true;
    prepared.magicgate_verified = true;

    auto frozen = bs::freeze_install_input(prepared, "target-fingerprint");
    check(frozen.ok && frozen.magicgate_verified,
          "verified FHDB preparation must retain verification evidence in frozen plan");

    std::string error;
    check(bs::validate_frozen_payload(frozen, error),
          "verified named bootstrap frozen plan must pass endpoint-independent validation");

    frozen.magicgate_verified = false;
    check(!bs::validate_frozen_payload(frozen, error),
          "tampering retained verification evidence must fail before physical admission");
}

void test_other_family_may_freeze_non_kelf_payload()
{
    namespace bs = ps2hdd::bootstrap;
    bs::PreparedBootstrap prepared;
    prepared.ok = true;
    prepared.manifest.family = bs::ProductFamily::other;
    prepared.manifest.provider_id = "generic";
    prepared.manifest.immutable_version = "v1";
    prepared.manifest.provenance = "synthetic generic fixture";
    prepared.payload.assign(1024U, std::byte{0x42});
    prepared.payload_sha256 = ps2hdd::crypto::sha256_hex(ps2hdd::crypto::sha256(prepared.payload));

    const auto frozen = bs::freeze_install_input(prepared, "target-fingerprint");
    check(frozen.ok, "explicit other provider may freeze a non-KELF payload without fake MagicGate evidence");
}

} // namespace

int main()
{
    try {
        test_inspection_only_named_product_cannot_freeze_for_install();
        test_verified_named_product_freezes_and_retains_evidence();
        test_other_family_may_freeze_non_kelf_payload();
        std::cout << "Bootstrap install-policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Bootstrap install-policy tests failed: " << error.what() << '\n';
        return 1;
    }
}
