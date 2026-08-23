#include "ps2hdd/magicgate_payload_known_vectors.hpp"
#include "ps2hdd/magicgate_service.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string hex_bytes(std::span<const std::byte> bytes)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2U);
    for (const auto value : bytes) {
        const auto byte = std::to_integer<unsigned>(value);
        out.push_back(digits[(byte >> 4U) & 0x0fU]);
        out.push_back(digits[byte & 0x0fU]);
    }
    return out;
}

std::string synthetic_keyset_text()
{
    namespace mg = ps2hdd::magicgate;
    const auto& keys = mg::known_vectors::kSyntheticKeyset;
    std::ostringstream out;
    out << "MG_SIG_MASTER_KEY=" << hex_bytes(keys.signing.signature_master) << '\n'
        << "MG_SIG_HASH_KEY=" << hex_bytes(keys.signing.signature_hash) << '\n'
        << "MG_KBIT_MASTER_KEY=" << hex_bytes(keys.disk.kbit_master) << '\n'
        << "MG_KBIT_MATERIAL=" << hex_bytes(keys.disk.kbit_material) << '\n'
        << "MG_KC_MASTER_KEY=" << hex_bytes(keys.disk.kc_master) << '\n'
        << "MG_KC_MATERIAL=" << hex_bytes(keys.disk.kc_material) << '\n'
        << "MG_ROOTSIG_MASTER_KEY=" << hex_bytes(keys.signing.root_signature_master) << '\n'
        << "MG_ROOTSIG_HASH_KEY=" << hex_bytes(keys.signing.root_signature_hash) << '\n'
        << "MG_CONTENT_TABLE_IV=" << hex_bytes(keys.signing.content_table_iv) << '\n'
        << "MG_CONTENT_IV=" << hex_bytes(keys.signing.content_iv) << '\n';
    return out.str();
}

ps2hdd::magicgate::DiskKelfSignPlan make_plan()
{
    namespace mg = ps2hdd::magicgate;
    mg::DiskKelfSignPlan plan;
    for (std::size_t i = 0; i < plan.header.user_header.size(); ++i) {
        plan.header.user_header[i] = static_cast<std::byte>(0x31U + i * 7U);
    }
    plan.header.unknown5 = 0x0701U;
    plan.header.flags = 0x0020U;
    plan.header.mg_zones = 1U;
    plan.content_keys.kbit = mg::known_vectors::kSyntheticKbit;
    plan.content_keys.kc = mg::known_vectors::kSyntheticKc;
    plan.signed_flag = 0x02U;
    plan.encrypted_flag = 0x01U;
    plan.blocks = {
        {32U, 0x03U},
        {32U, 0x02U},
        {32U, 0x01U},
    };
    return plan;
}

std::vector<std::byte> canonical_kelf()
{
    namespace mg = ps2hdd::magicgate;
    const auto signed_kelf = mg::sign_low_layout_disk_kelf(
        make_plan(), mg::payload_known_vectors::kPlaintextPayload,
        mg::known_vectors::kSyntheticKeyset);
    check(signed_kelf.ok, "canonical KELF fixture must sign");
    return signed_kelf.file;
}

void test_key_free_inspection_and_fail_closed_verify()
{
    namespace mg = ps2hdd::magicgate;
    mg::MagicGateHostService service;
    const auto file = canonical_kelf();

    const auto inspected = service.inspect(file);
    check(inspected.ok, "key-free host inspection must accept a structurally valid KELF");
    check(!inspected.keyset_loaded, "inspection must report that no keyset capability is loaded");

    const auto verified = service.verify(file);
    check(!verified.ok, "cryptographic host verification must fail closed without a keyset");
}

void test_keyset_parser_and_provenance_gate()
{
    namespace mg = ps2hdd::magicgate;
    mg::MagicGateHostService service;
    std::string error;

    check(!service.load_keyset(mg::known_vectors::kSyntheticKeyset, {}, error),
          "typed keyset injection must require provenance");
    check(!service.keyset_state().loaded, "failed keyset load must leave no capability behind");

    const auto text = synthetic_keyset_text();
    check(service.load_keyset_text(text, "synthetic-test-vector", error),
          "strict keyset text must load through the host service");
    check(service.keyset_state().loaded, "successful keyset load must publish capability state");
    check(service.keyset_state().provenance == "synthetic-test-vector",
          "host service must retain non-secret keyset provenance");
}

void test_complete_verify_and_plaintext_recovery()
{
    namespace mg = ps2hdd::magicgate;
    mg::MagicGateHostService service;
    std::string error;
    check(service.load_keyset(mg::known_vectors::kSyntheticKeyset,
                              "compiled synthetic test vector", error),
          "synthetic typed keyset must load");

    const auto file = canonical_kelf();
    const auto verified = service.verify(file);
    check(verified.ok, "host service must verify the complete canonical KELF");
    check(verified.envelope.header_signature_valid,
          "host verification must include the header signature");
    check(verified.envelope.bit_table_signature_valid,
          "host verification must include the BIT signature");
    check(verified.envelope.root_signature_valid,
          "host verification must include the root signature");
    check(verified.plaintext().size() == mg::payload_known_vectors::kPlaintextPayload.size(),
          "host verification plaintext size mismatch");
    check(std::equal(verified.plaintext().begin(), verified.plaintext().end(),
                     mg::payload_known_vectors::kPlaintextPayload.begin()),
          "host verification must recover exact plaintext bytes");
}

void test_service_sign_is_reverified()
{
    namespace mg = ps2hdd::magicgate;
    mg::MagicGateHostService service;
    std::string error;
    check(service.load_keyset(mg::known_vectors::kSyntheticKeyset,
                              "compiled synthetic test vector", error),
          "synthetic keyset must load before service signing");

    const auto result = service.sign(make_plan(), mg::payload_known_vectors::kPlaintextPayload);
    check(result.ok, "host service signing must succeed for the canonical plan");
    check(!result.file.empty(), "host service signing must return KELF bytes");
    check(result.verification.ok,
          "host service must independently reverify the exact bytes returned by its signer");
    check(result.verification.envelope.signed_flag_mapping == mg::SignedFlagMapping::bit_0x02,
          "service-signed KELF must prove the requested signed flag semantics");
}

void test_limits_are_enforced_before_crypto()
{
    namespace mg = ps2hdd::magicgate;
    mg::HostServiceLimits limits;
    limits.max_keyset_bytes = 32U;
    limits.max_kelf_bytes = 64U;
    limits.max_plaintext_bytes = 48U;
    mg::MagicGateHostService service(limits);
    std::string error;

    const auto text = synthetic_keyset_text();
    check(!service.load_keyset_text(text, "too-large-test", error),
          "oversized keyset text must be refused before parsing");

    check(service.load_keyset(mg::known_vectors::kSyntheticKeyset,
                              "typed-test-vector", error),
          "typed keyset should not be affected by the text-file byte limit");
    const auto file = canonical_kelf();
    check(!service.inspect(file).ok, "oversized KELF must be refused by key-free inspection");
    check(!service.verify(file).ok, "oversized KELF must be refused before cryptographic verification");
    check(!service.sign(make_plan(), mg::payload_known_vectors::kPlaintextPayload).ok,
          "oversized plaintext must be refused before signing");
}

void test_corruption_and_clear_keyset_fail_closed()
{
    namespace mg = ps2hdd::magicgate;
    mg::MagicGateHostService service;
    std::string error;
    check(service.load_keyset(mg::known_vectors::kSyntheticKeyset,
                              "compiled synthetic test vector", error),
          "synthetic keyset must load");

    auto file = canonical_kelf();
    const auto layout = mg::inspect_kelf(file);
    check(layout.ok, "canonical fixture layout must parse");
    file[layout.payload_offset + 5U] ^= std::byte{0x20};
    check(!service.verify(file).ok,
          "host service must reject payload corruption after envelope verification");

    service.clear_keyset();
    check(!service.keyset_state().loaded, "clear_keyset must revoke the host capability");
    check(service.keyset_state().provenance.empty(),
          "clear_keyset must discard provenance together with the capability");
    check(!service.verify(canonical_kelf()).ok,
          "verification must fail closed after the keyset capability is revoked");
}

} // namespace

int main()
{
    try {
        test_key_free_inspection_and_fail_closed_verify();
        test_keyset_parser_and_provenance_gate();
        test_complete_verify_and_plaintext_recovery();
        test_service_sign_is_reverified();
        test_limits_are_enforced_before_crypto();
        test_corruption_and_clear_keyset_fail_closed();
        std::cout << "MagicGate host service tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MagicGate host service tests failed: " << error.what() << '\n';
        return 1;
    }
}
