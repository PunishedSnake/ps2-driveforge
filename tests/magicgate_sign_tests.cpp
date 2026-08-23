#include "ps2hdd/magicgate_payload_known_vectors.hpp"
#include "ps2hdd/magicgate_sign.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ps2hdd::magicgate::DiskKelfSignPlan make_plan()
{
    namespace mg = ps2hdd::magicgate;

    mg::DiskKelfSignPlan plan;
    for (std::size_t i = 0; i < plan.header.user_header.size(); ++i) {
        plan.header.user_header[i] = static_cast<std::byte>(0x71U + i * 5U);
    }
    plan.header.unknown5 = 0x0701U;
    plan.header.flags = 0x0020U; // two-key content crypto, no variable field, no ICVPS2
    plan.header.mg_zones = 1U;
    plan.content_keys.kbit = mg::known_vectors::kSyntheticKbit;
    plan.content_keys.kc = mg::known_vectors::kSyntheticKc;
    plan.signed_flag = 0x02U;
    plan.encrypted_flag = 0x01U;
    plan.blocks = {
        {32U, 0x03U}, // signed + encrypted
        {32U, 0x02U}, // signed plaintext
        {32U, 0x01U}, // encrypted only
    };
    return plan;
}

void test_sign_verify_decrypt_round_trip()
{
    namespace mg = ps2hdd::magicgate;
    namespace vectors = mg::payload_known_vectors;

    const auto plan = make_plan();
    const auto& keyset = mg::known_vectors::kSyntheticKeyset;
    const auto signed_kelf = mg::sign_low_layout_disk_kelf(
        plan, vectors::kPlaintextPayload, keyset);

    check(signed_kelf.ok, "self-verifying MagicGate signer must accept the canonical synthetic plan");
    check(!signed_kelf.file.empty(), "successful MagicGate signing must return KELF bytes");
    check(signed_kelf.envelope.ok, "successful signer result must retain its verified envelope");
    check(signed_kelf.envelope.signed_flag_mapping == mg::SignedFlagMapping::bit_0x02,
          "signed output must prove the requested 0x02 signed selector through the root signature");

    const auto envelope = mg::verify_disk_kelf_header(signed_kelf.file, keyset);
    check(envelope.ok, "fresh verification of signed KELF envelope must succeed");
    const auto payload = mg::verify_and_decrypt_disk_kelf_payload(
        signed_kelf.file, envelope, keyset);
    check(payload.ok, "fresh verification of signed KELF payload must succeed");
    check(payload.plaintext.size() == vectors::kPlaintextPayload.size(),
          "signed KELF plaintext size mismatch");
    check(std::equal(payload.plaintext.begin(), payload.plaintext.end(),
                     vectors::kPlaintextPayload.begin()),
          "signed KELF must decrypt back to the exact source bytes");
}

void test_signing_is_deterministic_when_content_keys_are_frozen()
{
    namespace mg = ps2hdd::magicgate;
    namespace vectors = mg::payload_known_vectors;

    const auto plan = make_plan();
    const auto& keyset = mg::known_vectors::kSyntheticKeyset;
    const auto first = mg::sign_low_layout_disk_kelf(plan, vectors::kPlaintextPayload, keyset);
    const auto second = mg::sign_low_layout_disk_kelf(plan, vectors::kPlaintextPayload, keyset);

    check(first.ok && second.ok, "deterministic signing fixtures must both succeed");
    check(first.file == second.file,
          "frozen MagicGate plan and content keys must produce byte-identical KELF output");
}

void test_ambiguous_flag_plan_is_refused()
{
    namespace mg = ps2hdd::magicgate;
    namespace vectors = mg::payload_known_vectors;

    auto plan = make_plan();
    for (auto& block : plan.blocks) {
        block.flags = 0x03U;
    }

    const auto result = mg::sign_low_layout_disk_kelf(
        plan, vectors::kPlaintextPayload, mg::known_vectors::kSyntheticKeyset);
    check(!result.ok,
          "signer must refuse a plan where signed and encrypted membership cannot be distinguished");
}

void test_icvps2_is_fail_closed()
{
    namespace mg = ps2hdd::magicgate;
    namespace vectors = mg::payload_known_vectors;

    auto plan = make_plan();
    plan.header.flags |= 0x0002U;
    const auto result = mg::sign_low_layout_disk_kelf(
        plan, vectors::kPlaintextPayload, mg::known_vectors::kSyntheticKeyset);
    check(!result.ok,
          "signer must refuse ICVPS2 until the MechaCon 0x98 algorithm has a software oracle");
}

void test_unsupported_three_key_content_mode_is_refused()
{
    namespace mg = ps2hdd::magicgate;
    namespace vectors = mg::payload_known_vectors;

    auto plan = make_plan();
    plan.header.flags &= static_cast<std::uint16_t>(~0x0030U);
    plan.header.flags |= 0x0030U;
    const auto result = mg::sign_low_layout_disk_kelf(
        plan, vectors::kPlaintextPayload, mg::known_vectors::kSyntheticKeyset);
    check(!result.ok,
          "signer must not invent a third DES content key that does not exist in Kc");
}

void test_signed_output_corruption_is_detected()
{
    namespace mg = ps2hdd::magicgate;
    namespace vectors = mg::payload_known_vectors;

    const auto plan = make_plan();
    const auto& keyset = mg::known_vectors::kSyntheticKeyset;
    const auto signed_kelf = mg::sign_low_layout_disk_kelf(
        plan, vectors::kPlaintextPayload, keyset);
    check(signed_kelf.ok, "baseline signed KELF must succeed before corruption test");

    auto damaged = signed_kelf.file;
    const auto payload_offset = signed_kelf.envelope.layout.payload_offset;
    damaged[payload_offset + 11U] ^= std::byte{0x40};

    const auto envelope = mg::verify_disk_kelf_header(damaged, keyset);
    check(envelope.ok, "payload-only corruption must leave the header envelope independently valid");
    const auto payload = mg::verify_and_decrypt_disk_kelf_payload(damaged, envelope, keyset);
    check(!payload.ok, "signed payload corruption must be rejected after decryption");
}

} // namespace

int main()
{
    try {
        test_sign_verify_decrypt_round_trip();
        test_signing_is_deterministic_when_content_keys_are_frozen();
        test_ambiguous_flag_plan_is_refused();
        test_icvps2_is_fail_closed();
        test_unsupported_three_key_content_mode_is_refused();
        test_signed_output_corruption_is_detected();
        std::cout << "MagicGate host-side signer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MagicGate host-side signer tests failed: " << error.what() << '\n';
        return 1;
    }
}
