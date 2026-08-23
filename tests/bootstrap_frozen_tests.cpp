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

ps2hdd::bootstrap::FrozenInstallInput frozen_fixture()
{
    namespace bs = ps2hdd::bootstrap;
    namespace mg = ps2hdd::magicgate;

    mg::DiskKelfSignPlan sign_plan;
    for (std::size_t i = 0; i < sign_plan.header.user_header.size(); ++i)
        sign_plan.header.user_header[i] = static_cast<std::byte>(0x51U + i);
    sign_plan.header.unknown5 = 0x0701U;
    sign_plan.header.flags = 0x0020U;
    sign_plan.header.mg_zones = 1U;
    sign_plan.content_keys.kbit = mg::known_vectors::kSyntheticKbit;
    sign_plan.content_keys.kc = mg::known_vectors::kSyntheticKc;
    sign_plan.signed_flag = 0x02U;
    sign_plan.encrypted_flag = 0x01U;
    sign_plan.blocks = {{32U, 0x03U}, {32U, 0x02U}, {32U, 0x01U}};

    const auto signed_kelf = mg::sign_low_layout_disk_kelf(
        sign_plan, mg::payload_known_vectors::kPlaintextPayload,
        mg::known_vectors::kSyntheticKeyset);
    check(signed_kelf.ok, "frozen bootstrap test KELF must sign");

    bs::PreparedBootstrap prepared;
    prepared.ok = true;
    prepared.manifest.provider_id = "frozen-fixture";
    prepared.manifest.family = bs::ProductFamily::fhdb;
    prepared.manifest.immutable_version = "v1";
    prepared.manifest.provenance = "synthetic frozen fixture";
    prepared.source_sha256 = ps2hdd::crypto::sha256_hex(
        ps2hdd::crypto::sha256(signed_kelf.file));
    prepared.payload = signed_kelf.file;
    prepared.payload_sha256 = prepared.source_sha256;
    prepared.magicgate_inspected = true;
    prepared.magicgate_verified = true;
    return bs::freeze_install_input(prepared, "0123456789abcdef");
}

void test_valid_frozen_payload_passes()
{
    auto frozen = frozen_fixture();
    std::string error;
    check(frozen.ok, "frozen fixture must be created");
    check(ps2hdd::bootstrap::validate_frozen_payload(frozen, error),
          "unchanged frozen bootstrap payload must validate");
}

void test_payload_tamper_fails()
{
    auto frozen = frozen_fixture();
    frozen.payload.back() ^= std::byte{0x01};
    std::string error;
    check(!ps2hdd::bootstrap::validate_frozen_payload(frozen, error),
          "post-freeze payload mutation must fail hash validation");
}

void test_geometry_tamper_fails()
{
    auto frozen = frozen_fixture();
    ++frozen.payload_sector_count;
    std::string error;
    check(!ps2hdd::bootstrap::validate_frozen_payload(frozen, error),
          "post-freeze sector geometry mutation must fail validation");

    frozen = frozen_fixture();
    ++frozen.program_start_sector;
    check(!ps2hdd::bootstrap::validate_frozen_payload(frozen, error),
          "post-freeze __mbr start mutation must fail validation");
}

void test_trailing_kelf_bytes_fail_even_with_rehashed_payload()
{
    auto frozen = frozen_fixture();
    frozen.payload.push_back(std::byte{0});
    frozen.payload_sha256 = ps2hdd::crypto::sha256_hex(ps2hdd::crypto::sha256(frozen.payload));
    frozen.payload_sector_count = static_cast<std::uint32_t>((frozen.payload.size() + 511U) / 512U);
    std::string error;
    check(!ps2hdd::bootstrap::validate_frozen_payload(frozen, error),
          "named bootstrap KELF must reject unexpected trailing bytes even after a forged rehash");
}

void test_missing_target_identity_fails()
{
    auto frozen = frozen_fixture();
    frozen.target_fingerprint.clear();
    std::string error;
    check(!ps2hdd::bootstrap::validate_frozen_payload(frozen, error),
          "frozen bootstrap without target identity must fail before endpoint admission");
}

} // namespace

int main()
{
    try {
        test_valid_frozen_payload_passes();
        test_payload_tamper_fails();
        test_geometry_tamper_fails();
        test_trailing_kelf_bytes_fail_even_with_rehashed_payload();
        test_missing_target_identity_fails();
        std::cout << "Frozen bootstrap install-input tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Frozen bootstrap install-input tests failed: " << error.what() << '\n';
        return 1;
    }
}
