#include "ps2hdd/fhdb_artifacts.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path temp_root()
{
    const auto root = std::filesystem::temp_directory_path() / "ps2-driveforge-fhdb-artifacts-tests";
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
    check(static_cast<bool>(input), "open artifact for test readback");
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()),
                                   static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(input) || bytes.empty(), "read artifact bytes");
    return bytes;
}

ps2hdd::forensic::ScanResult sample_scan()
{
    ps2hdd::forensic::ScanResult scan;
    scan.ok = true;
    scan.total_sectors = 0x100000;
    scan.grid_reads = 4;
    scan.reference_reads = 1;
    scan.maps.push_back({});
    scan.maps[0].kind = ps2hdd::forensic::MapKind::forward;
    scan.maps[0].confidence = 98;
    scan.maps[0].repairable = true;
    scan.maps[0].order = {0, 1};

    ps2hdd::forensic::Node master;
    master.lba = 0;
    master.id = "__mbr";
    master.confidence = 100;
    master.header.fill(std::byte{0x11});
    scan.nodes.push_back(master);

    ps2hdd::forensic::Node node;
    node.lba = 0x40000;
    node.id = "+OPL";
    node.confidence = 95;
    node.header.fill(std::byte{0x22});
    scan.nodes.push_back(node);
    return scan;
}

ps2hdd::forensic::RepairPlan sample_plan()
{
    ps2hdd::forensic::RepairPlan plan;
    plan.map_index = 0;
    plan.confidence = 98;
    plan.corroborated_count = 1;
    plan.automatic_safe = true;
    ps2hdd::forensic::Patch patch;
    patch.node_index = 1;
    patch.lba = 0x40000;
    patch.old_next = 0x90000;
    patch.new_next = 0x80000;
    patch.old_prev = 0;
    patch.new_prev = 0;
    patch.checksum_corroborated = true;
    plan.patches.push_back(patch);
    return plan;
}

void test_apameta1_layout_and_validation()
{
    const auto scan = sample_scan();
    const auto plan = sample_plan();
    std::string error;
    auto bytes = ps2hdd::fhdb::build_hddmeta_image(scan, plan, error);
    check(!bytes.empty(), "build canonical APAMETA1 image");
    check(bytes.size() == 64 + 1060 + 32, "APAMETA1 exact file size mismatch");
    check(std::memcmp(bytes.data(), "APAMETA1", 8) == 0, "APAMETA1 magic mismatch");
    const auto valid = ps2hdd::fhdb::validate_hddmeta_image(bytes);
    check(valid.ok && valid.total_sectors == scan.total_sectors && valid.map_index == 0 &&
              valid.patch_count == 1 && valid.corroborated_count == 1 &&
              valid.speculative_count == 0 && valid.one_or_two_bit_count == 1,
          "APAMETA1 decoded fields mismatch");
    bytes.back() ^= std::byte{1};
    check(!ps2hdd::fhdb::validate_hddmeta_image(bytes).ok,
          "APAMETA1 trailer corruption must be rejected");
}

void test_two_slot_non_overwrite_policy()
{
    const auto root = temp_root();
    std::array<std::byte, 1024> first{};
    first.fill(std::byte{0x31});
    std::array<std::byte, 1024> second{};
    second.fill(std::byte{0x32});
    std::array<std::byte, 1024> third{};
    third.fill(std::byte{0x33});

    const auto a = ps2hdd::fhdb::save_hddraw(root, first);
    check(a.ok && !a.reused && a.path.filename() == "HDDRAW.BIN", "first HDDRAW slot mismatch");
    check(read_all(a.path) == std::vector<std::byte>(first.begin(), first.end()),
          "first HDDRAW bytes mismatch");

    const auto same = ps2hdd::fhdb::save_hddraw(root, first);
    check(same.ok && same.reused && same.path == a.path, "identical HDDRAW should reuse first slot");

    const auto b = ps2hdd::fhdb::save_hddraw(root, second);
    check(b.ok && !b.reused && b.path.filename() == "HDDRAW2.BIN", "second HDDRAW slot mismatch");
    const auto blocked = ps2hdd::fhdb::save_hddraw(root, third);
    check(!blocked.ok, "third different HDDRAW must not overwrite either canonical slot");
    std::filesystem::remove_all(root);
}

void test_hddmeta_and_forensic_report_storage()
{
    const auto root = temp_root();
    const auto scan = sample_scan();
    const auto plan = sample_plan();
    const auto meta = ps2hdd::fhdb::save_hddmeta(root, scan, plan);
    check(meta.ok && meta.path.filename() == "HDDMETA.BIN", "HDDMETA slot save failed");
    check(ps2hdd::fhdb::validate_hddmeta_image(read_all(meta.path)).ok,
          "saved HDDMETA failed canonical validation");

    const auto report = ps2hdd::fhdb::save_forensic_report(root, scan);
    check(report.ok && report.path.filename() == "FORENSIC.TXT", "FORENSIC.TXT save failed");
    const auto bytes = read_all(report.path);
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(text.starts_with("PS2 HDD Bootstrap Manager - APA FORENSIC REPORT\n\n"),
          "FORENSIC.TXT interoperability header mismatch");
    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    try {
        test_apameta1_layout_and_validation();
        test_two_slot_non_overwrite_policy();
        test_hddmeta_and_forensic_report_storage();
        std::cout << "FHDB artifact interoperability tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FHDB artifact interoperability tests failed: " << error.what() << '\n';
        return 1;
    }
}
