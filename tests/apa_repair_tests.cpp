#include "ps2hdd/apa_repair.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::uint16_t load_u16(const std::byte* p)
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[0])) |
           static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[1]) << 8U);
}
std::uint32_t load_u32(const std::byte* p)
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}
void store_u16(std::byte* p, std::uint16_t value)
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}
void store_u32(std::byte* p, std::uint32_t value)
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}
std::uint32_t checksum(std::span<const std::byte, 1024> header)
{
    std::uint32_t sum = 0;
    for (std::size_t i = 1; i < 256; ++i) sum += load_u32(header.data() + i * 4);
    return sum;
}
void checksum_header(std::array<std::byte, 1024>& header) { store_u32(header.data(), checksum(header)); }

std::array<std::byte, 1024> make_master(std::uint32_t osd_start = 0, std::uint32_t osd_size = 0)
{
    std::array<std::byte, 1024> header{};
    std::memcpy(header.data() + 0x004, "APA\0", 4);
    std::memcpy(header.data() + 0x010, "__mbr", 5);
    store_u32(header.data() + 0x040, 0);
    store_u32(header.data() + 0x044, 0x4000U);
    store_u16(header.data() + 0x048, 1);
    std::memcpy(header.data() + 0x100, "Sony Computer Entertainment Inc.", 32);
    store_u32(header.data() + 0x120, 2);
    store_u32(header.data() + 0x130, osd_start);
    store_u32(header.data() + 0x134, osd_size);
    checksum_header(header);
    return header;
}

void test_valid_and_checksum_only()
{
    auto valid = make_master();
    const auto valid_plan = ps2hdd::repair::analyze_apa_master(valid);
    check(valid_plan.issues == 0 && valid_plan.blockers == 0 && !valid_plan.header_patch_safe,
          "valid header produced repair plan");

    auto corrupted = make_master(0x2000, 1);
    corrupted[0x220] ^= std::byte{1};
    const auto plan = ps2hdd::repair::analyze_apa_master(corrupted);
    check(plan.issues == ps2hdd::repair::issue_checksum && !plan.header_patch_safe &&
              (plan.blockers & ps2hdd::repair::blocker_unexplained_checksum) != 0,
          "checksum-only corruption must remain diagnostic");
}

void test_known_identity_bitflip_and_collision()
{
    auto header = make_master();
    header[0x004] ^= std::byte{1};
    const auto plan = ps2hdd::repair::analyze_apa_master(header);
    check(plan.header_patch_safe && plan.blockers == 0 &&
              (plan.safe_header_fixes & ps2hdd::repair::issue_apa_magic) != 0,
          "single canonical identity bitflip should be repairable");
    std::array<std::byte, 1024> repaired{};
    std::string error;
    check(ps2hdd::repair::build_repaired_apa_master(header, plan, repaired, error),
          "repair builder rejected safe identity fix");
    check(std::memcmp(repaired.data() + 0x004, "APA\0", 4) == 0 &&
              load_u32(repaired.data()) == checksum(repaired),
          "repaired identity header is not canonical");

    header = make_master();
    header[0x004] ^= std::byte{1};
    header[0x220] ^= std::byte{1};
    check(load_u32(header.data()) == checksum(header), "collision fixture must preserve additive checksum");
    const auto collision = ps2hdd::repair::analyze_apa_master(header);
    check(!collision.header_patch_safe &&
              (collision.blockers & ps2hdd::repair::blocker_unexplained_checksum) != 0,
          "checksum collision must be refused");
}

void test_master_anchor_repairs_and_ambiguity()
{
    std::array<std::byte, 1024> repaired{};
    std::string error;

    auto type = make_master();
    store_u16(type.data() + 0x048, 0x1337);
    auto plan = ps2hdd::repair::analyze_apa_master(type);
    check(plan.header_patch_safe && (plan.safe_header_fixes & ps2hdd::repair::issue_master_type),
          "single master type corruption should be repairable");
    check(ps2hdd::repair::build_repaired_apa_master(type, plan, repaired, error) &&
              load_u16(repaired.data() + 0x048) == 1,
          "master type repair failed");

    auto version = make_master();
    store_u32(version.data() + 0x120, 99);
    plan = ps2hdd::repair::analyze_apa_master(version);
    check(plan.header_patch_safe && ps2hdd::repair::build_repaired_apa_master(version, plan, repaired, error) &&
              load_u32(repaired.data() + 0x120) == 2,
          "master version repair failed");

    auto ambiguous = make_master();
    ambiguous[0x004] ^= std::byte{1};
    ambiguous[0x010] ^= std::byte{1};
    plan = ps2hdd::repair::analyze_apa_master(ambiguous);
    check(!plan.header_patch_safe && (plan.blockers & ps2hdd::repair::blocker_low_identity),
          "multiple identity faults must be blocked");
}

void test_hybrid_pointer_and_torn_disable()
{
    auto hybrid = make_master();
    hybrid[0x1fe] = std::byte{0x55};
    hybrid[0x1ff] = std::byte{0xaa};
    checksum_header(hybrid);
    auto plan = ps2hdd::repair::analyze_apa_master(hybrid);
    check(!plan.header_patch_safe && (plan.blockers & ps2hdd::repair::blocker_hybrid_gpt),
          "hybrid APA/GPT must never be raw repaired");

    auto pointer = make_master(0x2000, 0);
    plan = ps2hdd::repair::analyze_apa_master(pointer);
    check(plan.pointer_clear_recommended && !plan.header_patch_safe &&
              (plan.issues & ps2hdd::repair::issue_pointer_inconsistent),
          "inconsistent bootstrap pointer should recommend clear only");

    auto torn = make_master(0x2000, 1);
    store_u32(torn.data() + 0x130, 0);
    store_u32(torn.data() + 0x134, 0);
    plan = ps2hdd::repair::analyze_apa_master(torn);
    check(!plan.header_patch_safe && plan.safe_header_fixes == 0 &&
              (plan.blockers & ps2hdd::repair::blocker_unexplained_checksum),
          "torn pointer disable must not manufacture a header repair");
}

} // namespace

int main()
{
    try {
        test_valid_and_checksum_only();
        test_known_identity_bitflip_and_collision();
        test_master_anchor_repairs_and_ambiguity();
        test_hybrid_pointer_and_torn_disable();
        std::cout << "FHDB-compatible APA repair planner tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "APA repair planner tests failed: " << error.what() << '\n';
        return 1;
    }
}
