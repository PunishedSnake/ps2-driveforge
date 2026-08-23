#include "ps2hdd/magicgate_payload_known_vectors.hpp"

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

void test_complete_synthetic_kelf_round_trip()
{
    namespace vectors = ps2hdd::magicgate::payload_known_vectors;
    namespace mg = ps2hdd::magicgate;

    const auto& keyset = mg::known_vectors::kParsedSyntheticKeyset.keyset;
    const auto envelope = mg::verify_disk_kelf_header(vectors::kCompleteDiskKelf, keyset);
    check(envelope.ok, "synthetic KELF header envelope must verify");
    check(envelope.signed_flag_mapping == mg::SignedFlagMapping::bit_0x02,
          "synthetic root signature must resolve 0x02 as the signed selector");

    const auto payload = mg::verify_and_decrypt_disk_kelf_payload(
        vectors::kCompleteDiskKelf, envelope, keyset);
    check(payload.ok, "synthetic KELF payload must decrypt and verify");
    check(payload.plaintext.size() == vectors::kPlaintextPayload.size(),
          "synthetic KELF plaintext size mismatch");
    check(std::equal(payload.plaintext.begin(), payload.plaintext.end(),
                     vectors::kPlaintextPayload.begin()),
          "synthetic KELF plaintext must round-trip byte-for-byte");
    check(payload.blocks.size() == 3U, "synthetic KELF must expose all three BIT blocks");
    check(payload.blocks[0].encrypted && payload.blocks[0].signed_block &&
              payload.blocks[0].signature_valid,
          "first synthetic block must be signed+encrypted and valid");
    check(!payload.blocks[1].encrypted && payload.blocks[1].signed_block &&
              payload.blocks[1].signature_valid,
          "second synthetic block must be signed plaintext and valid");
    check(payload.blocks[2].encrypted && !payload.blocks[2].signed_block,
          "third synthetic block must be encrypted-only");
}

void test_signed_encrypted_corruption_reaches_content_verifier()
{
    namespace vectors = ps2hdd::magicgate::payload_known_vectors;
    namespace mg = ps2hdd::magicgate;

    auto damaged = vectors::kCompleteDiskKelf;
    damaged[vectors::kHeaderBytes + 7U] ^= std::byte{0x20};
    const auto& keyset = mg::known_vectors::kParsedSyntheticKeyset.keyset;

    const auto envelope = mg::verify_disk_kelf_header(damaged, keyset);
    check(envelope.ok,
          "payload corruption must not masquerade as a header-envelope failure");
    const auto payload = mg::verify_and_decrypt_disk_kelf_payload(damaged, envelope, keyset);
    check(!payload.ok, "signed encrypted payload corruption must be rejected");
}

void test_signed_plaintext_corruption_reaches_content_verifier()
{
    namespace vectors = ps2hdd::magicgate::payload_known_vectors;
    namespace mg = ps2hdd::magicgate;

    auto damaged = vectors::kCompleteDiskKelf;
    damaged[vectors::kHeaderBytes + 32U + 9U] ^= std::byte{0x04};
    const auto& keyset = mg::known_vectors::kParsedSyntheticKeyset.keyset;

    const auto envelope = mg::verify_disk_kelf_header(damaged, keyset);
    check(envelope.ok,
          "signed plaintext corruption must leave the header envelope independently testable");
    const auto payload = mg::verify_and_decrypt_disk_kelf_payload(damaged, envelope, keyset);
    check(!payload.ok, "signed plaintext payload corruption must be rejected");
}

void test_wrong_content_iv_fails_without_breaking_header_envelope()
{
    namespace vectors = ps2hdd::magicgate::payload_known_vectors;
    namespace mg = ps2hdd::magicgate;

    auto keyset = mg::known_vectors::kParsedSyntheticKeyset.keyset;
    const auto envelope = mg::verify_disk_kelf_header(vectors::kCompleteDiskKelf, keyset);
    check(envelope.ok, "baseline envelope must verify before content-IV fault injection");

    keyset.signing.content_iv[0] ^= std::byte{0x80};
    const auto payload = mg::verify_and_decrypt_disk_kelf_payload(
        vectors::kCompleteDiskKelf, envelope, keyset);
    check(!payload.ok, "wrong content IV must fail payload verification");
}

} // namespace

int main()
{
    try {
        test_complete_synthetic_kelf_round_trip();
        test_signed_encrypted_corruption_reaches_content_verifier();
        test_signed_plaintext_corruption_reaches_content_verifier();
        test_wrong_content_iv_fails_without_breaking_header_envelope();
        std::cout << "MagicGate host-side KELF tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MagicGate host-side KELF tests failed: " << error.what() << '\n';
        return 1;
    }
}
