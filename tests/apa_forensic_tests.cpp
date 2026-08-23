#include "ps2hdd/apa_forensic.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
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
void finalize(std::array<std::byte, 1024>& header) { store_u32(header.data(), checksum(header)); }

std::array<std::byte, 1024> make_header(std::uint32_t lba, std::uint32_t length,
                                        std::uint32_t prev, std::uint32_t next,
                                        const char* id, std::uint16_t type)
{
    std::array<std::byte, 1024> header{};
    std::memcpy(header.data() + 0x004, "APA\0", 4);
    std::strncpy(reinterpret_cast<char*>(header.data() + 0x010), id, 32);
    store_u32(header.data() + 0x008, next);
    store_u32(header.data() + 0x00c, prev);
    store_u32(header.data() + 0x040, lba);
    store_u32(header.data() + 0x044, length);
    store_u16(header.data() + 0x048, type);
    finalize(header);
    return header;
}

std::array<std::byte, 1024> make_master(std::uint32_t prev, std::uint32_t next)
{
    auto header = make_header(0, 0x4000, prev, next, "__mbr", 1);
    std::memcpy(header.data() + 0x100, "Sony Computer Entertainment Inc.", 32);
    store_u32(header.data() + 0x120, 2);
    finalize(header);
    return header;
}

class SparseDisk final : public ps2hdd::BlockDevice {
public:
    explicit SparseDisk(std::uint32_t sectors) : sectors_(sectors) {}
    std::uint64_t size_bytes() const override { return static_cast<std::uint64_t>(sectors_) * 512ULL; }
    std::string display_name() const override { return "forensic-sparse"; }
    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > size_bytes() || out.size() > size_bytes() - offset) return false;
        std::fill(out.begin(), out.end(), std::byte{0});
        if (out.size() == 1024 && offset % 512 == 0) {
            const auto lba = static_cast<std::uint32_t>(offset / 512);
            const auto it = headers_.find(lba);
            if (it != headers_.end()) std::copy(it->second.begin(), it->second.end(), out.begin());
        }
        return true;
    }
    void put(std::uint32_t lba, std::array<std::byte, 1024> header) { headers_[lba] = std::move(header); }
private:
    std::uint32_t sectors_{};
    std::map<std::uint32_t, std::array<std::byte, 1024>> headers_;
};

const ps2hdd::forensic::Map* find_map(const ps2hdd::forensic::ScanResult& result,
                                      ps2hdd::forensic::MapKind kind)
{
    for (const auto& map : result.maps) if (map.kind == kind) return &map;
    return nullptr;
}

void test_healthy_graph_and_report()
{
    SparseDisk disk(0x100000);
    disk.put(0, make_master(0x80000, 0x40000));
    disk.put(0x40000, make_header(0x40000, 0x40000, 0, 0x80000, "__system", 0x0100));
    disk.put(0x80000, make_header(0x80000, 0x40000, 0x40000, 0, "+OPL", 0x0100));
    const auto result = ps2hdd::forensic::scan_apa(disk);
    check(result.ok && result.nodes.size() == 3, "healthy forensic scan node count mismatch");
    const auto* forward = find_map(result, ps2hdd::forensic::MapKind::forward);
    check(forward && forward->confidence == 100 && forward->repairable,
          "healthy forward map should be 100% repairable but require no patch");
    const auto index = static_cast<std::size_t>(forward - result.maps.data());
    const auto plan = ps2hdd::forensic::build_repair_plan(result, index);
    check(plan.patches.empty() && !plan.manual_allowed, "healthy map must not authorize writes");
    const auto report = ps2hdd::forensic::render_report(result);
    check(report.find("PS2 HDD Bootstrap Manager - APA FORENSIC REPORT") == 0 &&
              report.find("MAP 1:") != std::string::npos &&
              report.find("DISCOVERED HEADERS") != std::string::npos,
          "FORENSIC.TXT canonical schema missing");
}

void test_stale_checksum_link_repair_is_automatic()
{
    SparseDisk disk(0x100000);
    disk.put(0, make_master(0x80000, 0x40000));
    auto a = make_header(0x40000, 0x40000, 0, 0x80000, "A", 0x0100);
    // Change next but retain old checksum. B's reciprocal prev and geometry
    // independently identify the intended link, exactly like the FHDB vector.
    store_u32(a.data() + 0x008, 0x90000);
    disk.put(0x40000, a);
    disk.put(0x80000, make_header(0x80000, 0x40000, 0x40000, 0, "B", 0x0100));
    const auto result = ps2hdd::forensic::scan_apa(disk);
    const auto* forward = find_map(result, ps2hdd::forensic::MapKind::forward);
    check(forward && forward->repairable, "broken-link forward map should remain reconstructable");
    const auto plan = ps2hdd::forensic::build_repair_plan(
        result, static_cast<std::size_t>(forward - result.maps.data()));
    check(plan.patches.size() == 1 && plan.corroborated_count == 1 &&
              plan.speculative_count == 0 && plan.automatic_safe && plan.manual_allowed,
          "stale-checksum exact link repair should be automatic-safe");
    check(plan.patches[0].new_next == 0x80000, "repair plan inferred wrong next link");
    std::array<std::byte, 1024> repaired{};
    std::string error;
    check(ps2hdd::forensic::build_patched_header(result, plan.patches[0], repaired, error) &&
              load_u32(repaired.data() + 0x008) == 0x80000 &&
              load_u32(repaired.data()) == checksum(repaired),
          "forensic patched header did not restore next/checksum");
}

void test_checksummed_wrong_link_is_manual_only()
{
    SparseDisk disk(0x100000);
    disk.put(0, make_master(0x80000, 0x40000));
    auto a = make_header(0x40000, 0x40000, 0, 0x80000, "A", 0x0100);
    store_u32(a.data() + 0x008, 0x90000);
    finalize(a);
    disk.put(0x40000, a);
    disk.put(0x80000, make_header(0x80000, 0x40000, 0x40000, 0, "B", 0x0100));
    const auto result = ps2hdd::forensic::scan_apa(disk);
    const auto* forward = find_map(result, ps2hdd::forensic::MapKind::forward);
    check(forward != nullptr, "manual-only fixture missing forward map");
    const auto plan = ps2hdd::forensic::build_repair_plan(
        result, static_cast<std::size_t>(forward - result.maps.data()));
    check(plan.patches.size() == 1 && plan.corroborated_count == 0 &&
              plan.speculative_count == 1 && !plan.automatic_safe && plan.manual_allowed,
          "checksummed wrong link must require expert/manual path");
}

class GeneratedChain final : public ps2hdd::BlockDevice {
public:
    explicit GeneratedChain(std::uint32_t count)
        : count_(count), sectors_(count * ps2hdd::forensic::kScanStep + ps2hdd::forensic::kScanStep) {}
    std::uint64_t size_bytes() const override { return static_cast<std::uint64_t>(sectors_) * 512ULL; }
    std::string display_name() const override { return "forensic-generated-chain"; }
    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > size_bytes() || out.size() > size_bytes() - offset) return false;
        std::fill(out.begin(), out.end(), std::byte{0});
        if (out.size() != 1024 || offset % 512 != 0) return true;
        const auto lba = static_cast<std::uint32_t>(offset / 512);
        if (lba % ps2hdd::forensic::kScanStep != 0) return true;
        const auto index = lba / ps2hdd::forensic::kScanStep;
        if (index >= count_) return true;
        std::array<std::byte, 1024> header{};
        if (index == 0) {
            header = make_master((count_ - 1U) * ps2hdd::forensic::kScanStep,
                                 count_ > 1 ? ps2hdd::forensic::kScanStep : 0);
        } else {
            const auto prev = index == 1 ? 0 : (index - 1U) * ps2hdd::forensic::kScanStep;
            const auto next = index + 1U < count_ ? (index + 1U) * ps2hdd::forensic::kScanStep : 0;
            header = make_header(lba, ps2hdd::forensic::kScanStep, prev, next,
                                 "PP.TEST-LARGE-HDL", 0x1337);
        }
        std::copy(header.begin(), header.end(), out.begin());
        return true;
    }
private:
    std::uint32_t count_{};
    std::uint32_t sectors_{};
};

void test_truncated_scan_is_hard_read_only()
{
    GeneratedChain disk(static_cast<std::uint32_t>(ps2hdd::forensic::kMaxNodes + 8));
    const auto result = ps2hdd::forensic::scan_apa(disk);
    check(result.ok && result.truncated && result.nodes.size() == ps2hdd::forensic::kMaxNodes,
          "large generated chain should hit forensic node capacity");
    for (std::size_t i = 0; i < result.maps.size(); ++i) {
        const auto plan = ps2hdd::forensic::build_repair_plan(result, i);
        check(!plan.automatic_safe && !plan.manual_allowed && plan.patches.empty(),
              "truncated scan must never authorize a repair plan");
    }
    check(ps2hdd::forensic::render_report(result).find("Write planning: LOCKED") != std::string::npos,
          "truncated FORENSIC.TXT must report LOCKED write planning");
}

} // namespace

int main()
{
    try {
        test_healthy_graph_and_report();
        test_stale_checksum_link_repair_is_automatic();
        test_checksummed_wrong_link_is_manual_only();
        test_truncated_scan_is_hard_read_only();
        std::cout << "FHDB-compatible APA forensic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "APA forensic tests failed: " << error.what() << '\n';
        return 1;
    }
}
