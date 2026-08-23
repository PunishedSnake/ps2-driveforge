#include "ps2hdd/bootstrap_manifest.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string valid_manifest()
{
    return
        "provider_id=fhdb-example\n"
        "family=fhdb\n"
        "version=v1.2.3\n"
        "asset_url=https://example.invalid/releases/v1.2.3/MBR.KELF\n"
        "asset_name=MBR.KELF\n"
        "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
        "provenance=immutable fixture release\n"
        "magicgate=verify\n"
        "max_download_bytes=4194304\n";
}

void test_complete_manifest_parses()
{
    namespace bs = ps2hdd::bootstrap;
    const auto result = bs::parse_provider_manifest(valid_manifest());
    check(result.ok, "complete bootstrap provider manifest must parse");
    check(result.artifact.provider_id == "fhdb-example", "provider id mismatch");
    check(result.artifact.family == bs::ProductFamily::fhdb, "provider family mismatch");
    check(result.artifact.immutable_version == "v1.2.3", "provider version mismatch");
    check(result.artifact.magicgate_policy == bs::MagicGatePolicy::verify_kelf,
          "provider MagicGate policy mismatch");
    check(result.artifact.max_download_bytes == 4194304U, "provider byte limit mismatch");
}

void test_comments_and_aliases_parse()
{
    auto text = valid_manifest();
    text = "# local manifest\n; immutable inputs only\n" + text;
    auto result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(result.ok, "manifest comments must be ignored");

    text = valid_manifest();
    const auto pos = text.find("family=fhdb");
    text.replace(pos, std::string("family=fhdb").size(), "family=hdd_osd");
    result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(result.ok && result.artifact.family == ps2hdd::bootstrap::ProductFamily::hdd_osd,
          "hdd_osd family alias must parse");
}

void test_unknown_duplicate_and_missing_fields_fail()
{
    auto text = valid_manifest() + "surprise=true\n";
    auto result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(!result.ok && result.line != 0U, "unknown provider manifest field must fail with a line number");

    text = valid_manifest() + "provider_id=duplicate\n";
    result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(!result.ok && result.line != 0U, "duplicate provider manifest field must fail");

    text = valid_manifest();
    const auto begin = text.find("provenance=");
    const auto end = text.find('\n', begin);
    text.erase(begin, end - begin + 1U);
    result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(!result.ok, "missing required provider manifest field must fail");
}

void test_insecure_hash_and_policy_values_fail()
{
    auto text = valid_manifest();
    auto pos = text.find("https://");
    text.erase(pos, 1U);
    auto result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(!result.ok, "non-HTTPS provider URL must fail during manifest parsing");

    text = valid_manifest();
    pos = text.find("magicgate=verify");
    text.replace(pos, std::string("magicgate=verify").size(), "magicgate=none");
    result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(!result.ok, "named PS2 bootstrap family must refuse a policyless manifest");

    text = valid_manifest();
    pos = text.find("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    text.replace(pos, 64U, "deadbeef");
    result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(!result.ok, "short provider SHA-256 must fail during manifest parsing");
}

void test_invalid_download_bound_fails()
{
    auto text = valid_manifest();
    const auto pos = text.find("max_download_bytes=4194304");
    text.replace(pos, std::string("max_download_bytes=4194304").size(),
                 "max_download_bytes=999999999999999999999999");
    auto result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(!result.ok, "overflowing provider byte bound must fail");

    text = valid_manifest();
    const auto pos2 = text.find("max_download_bytes=4194304");
    text.replace(pos2, std::string("max_download_bytes=4194304").size(),
                 "max_download_bytes=0");
    result = ps2hdd::bootstrap::parse_provider_manifest(text);
    check(!result.ok, "zero provider byte bound must fail");
}

} // namespace

int main()
{
    try {
        test_complete_manifest_parses();
        test_comments_and_aliases_parse();
        test_unknown_duplicate_and_missing_fields_fail();
        test_insecure_hash_and_policy_values_fail();
        test_invalid_download_bound_fails();
        std::cout << "Bootstrap provider manifest tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Bootstrap provider manifest tests failed: " << error.what() << '\n';
        return 1;
    }
}
