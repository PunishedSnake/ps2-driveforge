#include "ps2hdd/apa_hdl_headers.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ps2hdd::apa::Ps2Time created_time()
{
    ps2hdd::apa::Ps2Time time{};
    time.sec = 6;
    time.min = 5;
    time.hour = 4;
    time.day = 3;
    time.month = 2;
    time.year = 2026;
    return time;
}

ps2hdd::apa::AllocationPlan sample_plan()
{
    ps2hdd::apa::AllocationPlan plan;
    plan.ok = true;
    plan.extents = {
        {2 * ps2hdd::apa::kAllocationChunkSectors,
         2 * ps2hdd::apa::kAllocationChunkSectors,
         ps2hdd::apa::kAllocationChunkSectors,
         4 * ps2hdd::apa::kAllocationChunkSectors},
        {4 * ps2hdd::apa::kAllocationChunkSectors,
         ps2hdd::apa::kAllocationChunkSectors,
         2 * ps2hdd::apa::kAllocationChunkSectors,
         0},
    };
    return plan;
}

void test_main_and_sub_headers()
{
    const auto plan = sample_plan();
    const auto created = created_time();
    const auto result = ps2hdd::apa::build_hdl_headers(
        plan, "PP.SLUS-12345..FRIEREN_TEST", created);

    check(result.ok, "valid HDL header plan should serialize");
    check(result.headers.size() == 2, "expected main plus one sub header");

    const auto& main = result.headers[0];
    const auto& sub = result.headers[1];
    check(main.magic == ps2hdd::apa::kMagic, "main APA magic mismatch");
    check(main.type == ps2hdd::apa::kTypeHdl && main.flags == 0, "main HDL type/flags mismatch");
    check(main.start == plan.extents[0].start_lba, "main start mismatch");
    check(main.length == plan.extents[0].length_sectors, "main length mismatch");
    check(main.prev == plan.extents[0].prev_lba && main.next == plan.extents[0].next_lba,
          "main chain links mismatch");
    check(main.nsub == 1 && main.main == 0 && main.number == 0, "main relationship fields mismatch");
    check(main.modver == 0x201, "main modver mismatch");
    check(main.subs[0].start == plan.extents[1].start_lba &&
              main.subs[0].length == plan.extents[1].length_sectors,
          "main sub allocation table mismatch");
    check(ps2hdd::apa::checksum(main) == main.checksum, "main checksum mismatch");
    check(std::memcmp(&main.created, &created, sizeof(created)) == 0, "main timestamp mismatch");

    check(sub.magic == ps2hdd::apa::kMagic, "sub APA magic mismatch");
    check(sub.type == ps2hdd::apa::kTypeHdl && sub.flags == ps2hdd::apa::kFlagSub,
          "sub HDL type/flags mismatch");
    check(sub.start == plan.extents[1].start_lba && sub.length == plan.extents[1].length_sectors,
          "sub extent mismatch");
    check(sub.prev == plan.extents[1].prev_lba && sub.next == plan.extents[1].next_lba,
          "sub chain links mismatch");
    check(sub.main == plan.extents[0].start_lba && sub.number == 1 && sub.nsub == 0,
          "sub relationship fields mismatch");
    check(sub.modver == 0x201, "sub modver mismatch");
    check(ps2hdd::apa::checksum(sub) == sub.checksum, "sub checksum mismatch");
}

void test_invalid_input_is_refused()
{
    auto bad = sample_plan();
    bad.ok = false;
    check(!ps2hdd::apa::build_hdl_headers(bad, "PP.TEST", created_time()).ok,
          "invalid allocation plan must be refused");

    const auto plan = sample_plan();
    check(!ps2hdd::apa::build_hdl_headers(plan, "", created_time()).ok,
          "empty partition id must be refused");
    check(!ps2hdd::apa::build_hdl_headers(plan, std::string(33, 'X'), created_time()).ok,
          "partition id over 32 bytes must be refused");
}

void test_maximum_sub_count()
{
    ps2hdd::apa::AllocationPlan plan;
    plan.ok = true;
    plan.extents.reserve(ps2hdd::apa::kMaxSub + 1);
    for (std::uint32_t i = 0; i <= ps2hdd::apa::kMaxSub; ++i) {
        const auto start = (i + 1) * ps2hdd::apa::kAllocationChunkSectors;
        plan.extents.push_back({start, ps2hdd::apa::kAllocationChunkSectors,
                                i == 0 ? 0 : i * ps2hdd::apa::kAllocationChunkSectors,
                                i == ps2hdd::apa::kMaxSub
                                    ? 0
                                    : (i + 2) * ps2hdd::apa::kAllocationChunkSectors});
    }

    const auto result = ps2hdd::apa::build_hdl_headers(plan, "PP.MAX-SUBS..TEST", created_time());
    check(result.ok, "1 main + 64 sub headers should be accepted");
    check(result.headers.size() == 65, "maximum legal HDL allocation should produce 65 headers");
    check(result.headers.front().nsub == 64, "maximum main header should declare 64 subs");
    check(result.headers.back().number == 64, "last sub number should be 64");
}

} // namespace

int main()
{
    try {
        test_main_and_sub_headers();
        test_invalid_input_is_refused();
        test_maximum_sub_count();
        std::cout << "APA HDL header tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "APA HDL header tests failed: " << error.what() << '\n';
        return 1;
    }
}
