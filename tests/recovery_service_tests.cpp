#include "ps2hdd/recovery_service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace rs = ps2hdd::recovery_service;
using ps2hdd::health::Action;
using ps2hdd::health::Verdict;

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void put32(std::vector<std::byte>& d, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) d[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xffU);
}

std::uint32_t get32(const std::vector<std::byte>& d, std::size_t at)
{
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | std::to_integer<std::uint32_t>(d[at + i]);
    return v;
}

void seal(std::vector<std::byte>& d)
{
    std::uint32_t sum = 0;
    for (std::size_t w = 1; w < 256; ++w) sum += get32(d, w * 4);
    put32(d, 0, sum);
}

std::filesystem::path make_image(const std::filesystem::path& dir, std::uint32_t start, std::uint32_t size,
                                 bool kelf)
{
    std::vector<std::byte> d(0x4000U * 512U);
    std::memcpy(d.data() + 4, "APA\0", 4);
    std::memcpy(d.data() + 0x10, "__mbr", 5);
    put32(d, 0x44, 0x4000);
    d[0x48] = std::byte{1};
    std::memcpy(d.data() + 0x100, "Sony Computer Entertainment Inc.", 32);
    put32(d, 0x120, 2);
    put32(d, 0x130, start);
    put32(d, 0x134, size);
    seal(d);
    if (kelf) {
        const std::size_t p = 0x2000U * 512U;
        put32(d, p + 0x10, 8);
        d[p + 0x14] = std::byte{72};
        for (std::size_t i = 0; i < 8; ++i) d[p + 72 + i] = static_cast<std::byte>(0x30 + i);
    }
    const auto path = dir / "disk.img";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(d.data()),
                                                static_cast<std::streamsize>(d.size()));
    return path;
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>(in), {}};
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

std::filesystem::path fresh(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() / (std::string("ps2df-recovery-service-") + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

void broken_payload_round_trip()
{
    const auto dir = fresh("broken");
    rs::Source source;
    source.image = make_image(dir, 0x2000, 1, false);
    check(!rs::Source{}.valid() && source.valid() && source.name() == "disk.img", "source identity");

    const auto before = rs::assess(source);
    check(before.ok && before.report.verdict == Verdict::repairable &&
          ps2hdd::health::recommends(before.report, Action::disable_bootstrap), "service health recommends disable");
    check(!rs::disable_bootstrap(source, {}).ok, "safety directory mandatory");

    const auto off = rs::disable_bootstrap(source, dir / "safety");
    if (!off.ok) std::cerr << off.error << '\n';
    check(off.ok && off.artifacts.size() == 2 && off.cold_verified, "disable saves HDDMBR + HDDRESCUE");
    const auto image = slurp(source.image);
    check(get32(image, 0x130) == 0 && get32(image, 0x134) == 0, "image pointer cleared on disk");
    check(rs::assess(source).report.verdict == Verdict::healthy, "healthy after disable");
    check(rs::disable_bootstrap(source, dir / "safety").nothing_to_do, "second disable is a no-op");

    const auto restored = rs::restore_bootstrap(source, dir / "safety", dir / "safety2");
    check(!restored.ok && get32(slurp(source.image), 0x134) == 0, "non-KELF payload is never republished");

    const auto capsule = rs::capture_rescue(source, dir / "capsule");
    check(capsule.ok && capsule.summary.find("Header-only") != std::string::npos, "header-only capsule when disabled");
}

void good_payload_backup_disable_restore()
{
    const auto dir = fresh("good");
    rs::Source source;
    source.image = make_image(dir, 0x2000, 1, true);
    check(rs::assess(source).report.verdict == Verdict::healthy, "valid KELF disk is healthy");
    const auto off = rs::disable_bootstrap(source, dir / "safety");
    check(off.ok, "disable a good bootstrap");
    const auto restored = rs::restore_bootstrap(source, dir / "safety", dir / "safety2");
    if (!restored.ok) std::cerr << restored.error << '\n';
    check(restored.ok, "a valid capsule restores the pointer");
    const auto image = slurp(source.image);
    check(get32(image, 0x130) == 0x2000 && get32(image, 0x134) == 1, "pointer republished");
}

void master_repair_then_forensic()
{
    const auto dir = fresh("master");
    rs::Source source;
    source.image = make_image(dir, 0x2000, 1, true);
    {
        auto bytes = slurp(source.image);
        bytes[4] = std::byte{'B'};
        std::ofstream(source.image, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto damaged = rs::assess(source);
    check(!damaged.report.actions.empty() && damaged.report.actions[0] == Action::repair_master, "master repair first");
    const auto repaired = rs::repair_master(source, dir / "safety");
    if (!repaired.ok) std::cerr << repaired.error << '\n';
    check(repaired.ok && !repaired.artifacts.empty(), "service master repair with HDDRAW");
    check(rs::assess(source).report.verdict == Verdict::healthy, "healthy after master repair");

    const auto report = rs::forensic_report(source, dir / "forensic");
    check(report.ok && std::filesystem::exists(dir / "forensic" / "FORENSIC.TXT"), "forensic report exported");
    check(!rs::repair_forensic_automatic(source, dir / "forensic").ok, "no automatic-safe map on a healthy disk");
}

} // namespace

int main()
{
    try {
        broken_payload_round_trip();
        good_payload_backup_disable_restore();
        master_repair_then_forensic();
    } catch (const std::exception& error) {
        std::cerr << "recovery service test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Recovery service tests passed\n";
    return 0;
}
