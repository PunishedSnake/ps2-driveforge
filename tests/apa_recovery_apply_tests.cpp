#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/apa_recovery_apply.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

class SparseWritableDisk final : public ps2hdd::WritableBlockDevice {
public:
    explicit SparseWritableDisk(std::uint32_t sectors) : sectors_(sectors) {}
    std::uint64_t size_bytes() const override { return static_cast<std::uint64_t>(sectors_) * 512ULL; }
    std::string display_name() const override { return "recovery-sparse.img"; }
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
    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        if (offset > size_bytes() || in.size() > size_bytes() - offset ||
            in.size() != 1024 || offset % 512 != 0) return false;
        const auto lba = static_cast<std::uint32_t>(offset / 512);
        std::array<std::byte, 1024> header{};
        std::copy(in.begin(), in.end(), header.begin());
        headers_[lba] = header;
        write_lbas.push_back(lba);
        return true;
    }
    bool flush() override { ++flushes; return true; }
    void put(std::uint32_t lba, std::array<std::byte, 1024> header) { headers_[lba] = std::move(header); }
    const std::array<std::byte, 1024>& at(std::uint32_t lba) const { return headers_.at(lba); }
    std::vector<std::uint32_t> write_lbas;
    std::size_t flushes{};
private:
    std::uint32_t sectors_{};
    std::map<std::uint32_t, std::array<std::byte, 1024>> headers_;
};

std::filesystem::path temp_root(const char* suffix)
{
    const auto root = std::filesystem::temp_directory_path() /
                      (std::string("ps2-driveforge-apa-recovery-") + suffix);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

std::vector<std::byte> read_all(const std::filesystem::path& path)
{
    const auto size = std::filesystem::file_size(path);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    check(static_cast<bool>(input), "open recovery artifact");
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(input) || bytes.empty(), "read recovery artifact");
    return bytes;
}

void test_master_repair_preserves_exact_hddraw()
{
    SparseWritableDisk disk(0x100000);
    auto damaged = make_master(0, 0);
    damaged[0x004] ^= std::byte{1}; // leave old checksum as corroboration
    disk.put(0, damaged);
    const auto root = temp_root("master");
    const auto result = ps2hdd::recovery::repair_master_header(disk, root);
    check(result.ok && result.writes_completed == 1 && !result.partial,
          "conservative master repair should succeed");
    check(result.snapshot_path.filename() == "HDDRAW.BIN", "master repair must save HDDRAW first");
    check(read_all(result.snapshot_path) == std::vector<std::byte>(damaged.begin(), damaged.end()),
          "HDDRAW must preserve exact damaged 1024-byte master");
    check(std::memcmp(disk.at(0).data() + 0x004, "APA\0", 4) == 0 &&
              load_u32(disk.at(0).data()) == checksum(disk.at(0)),
          "master repair did not restore canonical magic/checksum");
    std::filesystem::remove_all(root);
}

void test_forensic_repair_saves_artifacts_and_writes_master_last()
{
    SparseWritableDisk disk(0x100000);
    auto master = make_master(0x80000, 0x40000);
    // Two physical link corruptions retain the original checksums. The still
    // valid master->A link, B->A reciprocal link and geometry reconstruct the
    // same M,A,B chain, while each exact correction restores its stale checksum.
    // This produces two corroborated automatic patches and exercises LBA0-last.
    store_u32(master.data() + 0x00c, 0x90000);
    disk.put(0, master);
    auto a = make_header(0x40000, 0x40000, 0, 0x80000, "A", 0x0100);
    store_u32(a.data() + 0x008, 0x90000); // do NOT recompute checksum
    disk.put(0x40000, a);
    disk.put(0x80000, make_header(0x80000, 0x40000, 0x40000, 0, "B", 0x0100));

    const auto scan = ps2hdd::forensic::scan_apa(disk);
    check(scan.ok && !scan.truncated, "forensic repair fixture scan failed");
    std::size_t selected = scan.maps.size();
    ps2hdd::forensic::RepairPlan plan;
    for (std::size_t i = 0; i < scan.maps.size(); ++i) {
        auto candidate = ps2hdd::forensic::build_repair_plan(scan, i);
        if (candidate.automatic_safe && candidate.patches.size() >= 2) {
            selected = i;
            plan = std::move(candidate);
            break;
        }
    }
    check(selected < scan.maps.size(), "fixture should produce automatic multi-header repair plan");
    check(plan.corroborated_count == plan.patches.size() && plan.speculative_count == 0,
          "automatic multi-header fixture must be checksum-corroborated, not heuristic");

    const auto root = temp_root("forensic");
    const auto result = ps2hdd::recovery::repair_forensic_topology(disk, scan, plan, root);
    check(result.ok && !result.partial && result.writes_completed == plan.patches.size(),
          "automatic forensic topology repair failed");
    check(result.snapshot_path.filename() == "HDDMETA.BIN" &&
              result.forensic_report_path.filename() == "FORENSIC.TXT",
          "forensic repair must preserve canonical artifacts");
    check(!disk.write_lbas.empty() && disk.write_lbas.back() == 0,
          "forensic repair must write master LBA0 last");
    check(ps2hdd::fhdb::validate_hddmeta_image(read_all(result.snapshot_path)).ok,
          "generated HDDMETA must validate after repair");
    const auto report_bytes = read_all(result.forensic_report_path);
    const std::string report(reinterpret_cast<const char*>(report_bytes.data()), report_bytes.size());
    check(report.starts_with("PS2 HDD Bootstrap Manager - APA FORENSIC REPORT"),
          "FORENSIC.TXT must stay cross-tool compatible");
    std::filesystem::remove_all(root);
}

void test_speculative_plan_requires_manual_authorization()
{
    SparseWritableDisk disk(0x100000);
    disk.put(0, make_master(0x80000, 0x40000));
    auto a = make_header(0x40000, 0x40000, 0, 0x80000, "A", 0x0100);
    store_u32(a.data() + 0x008, 0x90000);
    finalize(a); // internally checksummed wrong link => speculative/manual-only
    disk.put(0x40000, a);
    disk.put(0x80000, make_header(0x80000, 0x40000, 0x40000, 0, "B", 0x0100));
    const auto scan = ps2hdd::forensic::scan_apa(disk);
    ps2hdd::forensic::RepairPlan plan;
    for (std::size_t i = 0; i < scan.maps.size(); ++i) {
        auto candidate = ps2hdd::forensic::build_repair_plan(scan, i);
        if (candidate.manual_allowed && !candidate.automatic_safe) {
            plan = std::move(candidate);
            break;
        }
    }
    check(plan.manual_allowed && !plan.automatic_safe, "manual-only forensic plan fixture failed");
    const auto root = temp_root("manual");
    const auto refused = ps2hdd::recovery::repair_forensic_topology(disk, scan, plan, root, false);
    check(!refused.ok && disk.write_lbas.empty(), "speculative plan must be refused without manual gate");
    const auto applied = ps2hdd::recovery::repair_forensic_topology(disk, scan, plan, root, true);
    check(applied.ok && !disk.write_lbas.empty(), "explicit manual forensic repair should apply");
    std::filesystem::remove_all(root);
}

void test_truncated_scan_never_writes()
{
    SparseWritableDisk disk(0x100000);
    ps2hdd::forensic::ScanResult scan;
    scan.ok = true;
    scan.truncated = true;
    scan.total_sectors = 0x100000;
    scan.maps.push_back({});
    ps2hdd::forensic::RepairPlan plan;
    plan.map_index = 0;
    plan.manual_allowed = true;
    ps2hdd::forensic::Patch patch;
    plan.patches.push_back(patch);
    const auto root = temp_root("truncated");
    const auto result = ps2hdd::recovery::repair_forensic_topology(disk, scan, plan, root, true);
    check(!result.ok && disk.write_lbas.empty(), "truncated forensic map must stay hard read-only");
    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    try {
        test_master_repair_preserves_exact_hddraw();
        test_forensic_repair_saves_artifacts_and_writes_master_last();
        test_speculative_plan_requires_manual_authorization();
        test_truncated_scan_never_writes();
        std::cout << "Guarded APA recovery execution tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "APA recovery execution tests failed: " << error.what() << '\n';
        return 1;
    }
}
