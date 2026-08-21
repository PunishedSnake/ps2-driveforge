#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_export.hpp"
#include "test_sha256.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ps2hdd::apa::kSectorSize;
constexpr std::uint32_t kZoneSize = 8192;
constexpr std::uint32_t kMainLba = 0x4000;
constexpr std::uint32_t kMainLength = 0x10000;
constexpr std::uint32_t kSubLba = 0x18000;
constexpr std::uint32_t kSubLength = 0x8000;
constexpr std::uint64_t kImageBytes = static_cast<std::uint64_t>(kSubLba + kSubLength) * kSectorSize;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void write_at(std::fstream& image, std::uint64_t offset, std::span<const std::byte> data)
{
    image.seekp(static_cast<std::streamoff>(offset));
    check(static_cast<bool>(image), "seek generated image");
    image.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    check(static_cast<bool>(image), "write generated image");
}

template <typename T>
void write_object(std::fstream& image, std::uint64_t offset, const T& value)
{
    write_at(image, offset, std::as_bytes(std::span{&value, 1}));
}

ps2hdd::apa::Header make_header(std::string_view id, std::uint16_t type, std::uint32_t start,
                                std::uint32_t length, std::uint32_t prev, std::uint32_t next)
{
    ps2hdd::apa::Header h{};
    h.magic = ps2hdd::apa::kMagic;
    std::memcpy(h.id, id.data(), std::min(id.size(), sizeof(h.id) - 1U));
    h.type = type;
    h.start = start;
    h.length = length;
    h.prev = prev;
    h.next = next;
    h.mbr.version = 2;
    h.checksum = ps2hdd::apa::checksum(h);
    return h;
}

std::vector<std::byte> pattern(std::size_t size, unsigned seed)
{
    std::vector<std::byte> result(size);
    for (std::size_t i = 0; i < size; ++i) {
        result[i] = static_cast<std::byte>((i * 37U + seed * 53U + (i >> 8U)) & 0xFFU);
    }
    return result;
}

std::uint16_t allocated_name_size(std::string_view name)
{
    return static_cast<std::uint16_t>((8U + name.size() + 3U) & ~3U);
}

class DirectorySector {
public:
    DirectorySector(ps2hdd::pfs::BlockInfo self, ps2hdd::pfs::BlockInfo parent)
    {
        add(self, ".", ps2hdd::pfs::kModeDirectory);
        add(parent, "..", ps2hdd::pfs::kModeDirectory);
    }

    void add(ps2hdd::pfs::BlockInfo inode, std::string_view name, std::uint16_t mode)
    {
        const auto allocated = allocated_name_size(name);
        check(offset_ + allocated <= bytes_.size(), "generated directory fits one sector");
        std::memcpy(bytes_.data() + static_cast<std::ptrdiff_t>(offset_), &inode.number, sizeof(inode.number));
        bytes_[offset_ + 4] = static_cast<std::byte>(inode.subpart);
        bytes_[offset_ + 5] = static_cast<std::byte>(name.size());
        const auto raw = static_cast<std::uint16_t>(mode | allocated);
        std::memcpy(bytes_.data() + static_cast<std::ptrdiff_t>(offset_ + 6), &raw, sizeof(raw));
        std::memcpy(bytes_.data() + static_cast<std::ptrdiff_t>(offset_ + 8), name.data(), name.size());
        offset_ += allocated;
    }

    [[nodiscard]] std::array<std::byte, kSectorSize> finish()
    {
        if (offset_ < bytes_.size()) {
            const auto remaining = static_cast<std::uint16_t>(bytes_.size() - offset_);
            check(remaining >= 8 && (remaining & 3U) == 0, "generated free dentry is aligned");
            std::memcpy(bytes_.data() + static_cast<std::ptrdiff_t>(offset_ + 6), &remaining, sizeof(remaining));
        }
        return bytes_;
    }

private:
    std::array<std::byte, kSectorSize> bytes_{};
    std::size_t offset_{};
};

ps2hdd::pfs::Inode make_inode(ps2hdd::pfs::BlockInfo location, std::uint16_t mode,
                              std::uint64_t size, std::span<const ps2hdd::pfs::BlockInfo> extents)
{
    ps2hdd::pfs::Inode inode{};
    inode.magic = ps2hdd::pfs::kSegdMagic;
    inode.inode_block = location;
    inode.last_segment = location;
    inode.data[0] = location;
    inode.mode = static_cast<std::uint16_t>(mode | (mode == ps2hdd::pfs::kModeDirectory ? 0x01FF : 0x01B6));
    inode.size = size;
    check(extents.size() <= ps2hdd::pfs::kInodeMaxBlocks - 1, "direct extent count fits SEGD");
    for (std::size_t i = 0; i < extents.size(); ++i) {
        inode.data[i + 1] = extents[i];
    }
    inode.number_data = static_cast<std::uint32_t>(1 + extents.size());
    inode.number_blocks = inode.number_data;
    inode.checksum = ps2hdd::pfs::inode_checksum(inode);
    return inode;
}

std::uint64_t volume_offset(std::uint64_t base, ps2hdd::pfs::BlockInfo block)
{
    return base + static_cast<std::uint64_t>(block.number) * kZoneSize;
}

struct ExpectedFile {
    std::filesystem::path host_relative;
    std::vector<std::byte> contents;
};

void write_regular(std::fstream& image, std::uint64_t main_base, std::uint64_t sub_base,
                   ps2hdd::pfs::BlockInfo inode_location,
                   ps2hdd::pfs::BlockInfo data_location,
                   std::span<const std::byte> contents)
{
    const std::array extents{data_location};
    const auto inode = make_inode(inode_location, ps2hdd::pfs::kModeRegular, contents.size(), extents);
    const auto inode_base = inode_location.subpart == 0 ? main_base : sub_base;
    const auto data_base = data_location.subpart == 0 ? main_base : sub_base;
    write_object(image, volume_offset(inode_base, inode_location), inode);
    write_at(image, volume_offset(data_base, data_location), contents);
}

std::vector<ExpectedFile> generate_image(const std::filesystem::path& path)
{
    std::fstream image(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    check(static_cast<bool>(image), "create generated image");
    image.seekp(static_cast<std::streamoff>(kImageBytes - 1));
    image.put('\0');
    check(static_cast<bool>(image), "size generated image");

    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, kMainLba, 0, kMainLba);
    constexpr char sony[] = "Sony Computer Entertainment Inc.";
    std::memcpy(mbr.mbr.magic, sony, std::min(sizeof(sony), sizeof(mbr.mbr.magic)));
    mbr.checksum = ps2hdd::apa::checksum(mbr);

    auto main = make_header("+TEST", ps2hdd::apa::kTypePfs, kMainLba, kMainLength, 0, kSubLba);
    main.nsub = 1;
    main.subs[0] = {kSubLba, kSubLength};
    main.checksum = ps2hdd::apa::checksum(main);

    auto sub = make_header("+TEST-sub", ps2hdd::apa::kTypePfs, kSubLba, kSubLength, kMainLba, 0);
    sub.flags = ps2hdd::apa::kFlagSub;
    sub.main = kMainLba;
    sub.number = 1;
    sub.checksum = ps2hdd::apa::checksum(sub);

    write_object(image, 0, mbr);
    write_object(image, static_cast<std::uint64_t>(kMainLba) * kSectorSize, main);
    write_object(image, static_cast<std::uint64_t>(kSubLba) * kSectorSize, sub);

    const auto main_base = static_cast<std::uint64_t>(kMainLba) * kSectorSize;
    const auto sub_base = static_cast<std::uint64_t>(kSubLba) * kSectorSize;

    constexpr ps2hdd::pfs::BlockInfo root{700,0,1};
    constexpr ps2hdd::pfs::BlockInfo root_data{701,0,1};
    constexpr ps2hdd::pfs::BlockInfo empty{702,0,1};
    constexpr ps2hdd::pfs::BlockInfo empty_data{703,0,1};
    constexpr ps2hdd::pfs::BlockInfo nested{704,0,1};
    constexpr ps2hdd::pfs::BlockInfo nested_data{705,0,1};
    constexpr ps2hdd::pfs::BlockInfo level1{706,0,1};
    constexpr ps2hdd::pfs::BlockInfo level1_data{707,0,1};
    constexpr ps2hdd::pfs::BlockInfo level2{708,0,1};
    constexpr ps2hdd::pfs::BlockInfo level2_data{709,0,1};
    constexpr ps2hdd::pfs::BlockInfo nested_file{710,0,1};
    constexpr ps2hdd::pfs::BlockInfo nested_file_data{711,0,1};
    constexpr ps2hdd::pfs::BlockInfo small{712,0,1};
    constexpr ps2hdd::pfs::BlockInfo small_data{713,0,1};
    constexpr ps2hdd::pfs::BlockInfo boundary{714,0,1};
    constexpr ps2hdd::pfs::BlockInfo boundary_data{715,0,1};
    constexpr ps2hdd::pfs::BlockInfo fragmented{100,1,1};
    constexpr ps2hdd::pfs::BlockInfo fragmented_main{716,0,1};
    constexpr ps2hdd::pfs::BlockInfo fragmented_sub{110,1,1};
    constexpr ps2hdd::pfs::BlockInfo con{717,0,1};
    constexpr ps2hdd::pfs::BlockInfo con_data{718,0,1};
    constexpr ps2hdd::pfs::BlockInfo foo{719,0,1};
    constexpr ps2hdd::pfs::BlockInfo foo_data{720,0,1};
    constexpr ps2hdd::pfs::BlockInfo upper_foo{721,0,1};
    constexpr ps2hdd::pfs::BlockInfo upper_foo_data{722,0,1};
    constexpr ps2hdd::pfs::BlockInfo bad_name{723,0,1};
    constexpr ps2hdd::pfs::BlockInfo bad_name_data{724,0,1};
    constexpr ps2hdd::pfs::BlockInfo segi_inode{940,0,1};
    constexpr ps2hdd::pfs::BlockInfo segi_meta{950,0,1};

    ps2hdd::pfs::SuperBlock super{};
    super.magic = ps2hdd::pfs::kSuperMagic;
    super.version = 3;
    super.zone_size = kZoneSize;
    super.num_subs = 1;
    super.log = {650,0,1};
    super.root = root;
    std::array<std::byte, kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &super, sizeof(super));
    write_at(image, main_base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * kSectorSize, super_sector);
    write_at(image, main_base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperBackupSector) * kSectorSize, super_sector);

    const auto nested_bytes = pattern(333, 1);
    const auto small_bytes = pattern(37, 2);
    const auto boundary_bytes = pattern(1024, 3);
    const auto fragmented_bytes = pattern(kZoneSize + 1337U, 4);
    const auto con_bytes = pattern(5, 5);
    const auto foo_bytes = pattern(9, 6);
    const auto upper_foo_bytes = pattern(11, 7);
    const auto bad_name_bytes = pattern(13, 8);
    const auto segi_bytes = pattern(114U * kZoneSize, 9);

    DirectorySector root_dir(root, root);
    root_dir.add(empty, "EMPTY", ps2hdd::pfs::kModeDirectory);
    root_dir.add(nested, "nested", ps2hdd::pfs::kModeDirectory);
    root_dir.add(small, "small.txt", ps2hdd::pfs::kModeRegular);
    root_dir.add(boundary, "boundary.bin", ps2hdd::pfs::kModeRegular);
    root_dir.add(fragmented, "fragmented.bin", ps2hdd::pfs::kModeRegular);
    root_dir.add(segi_inode, "segi.bin", ps2hdd::pfs::kModeRegular);
    root_dir.add(con, "CON", ps2hdd::pfs::kModeRegular);
    root_dir.add(foo, "foo", ps2hdd::pfs::kModeRegular);
    root_dir.add(upper_foo, "FOO", ps2hdd::pfs::kModeRegular);
    root_dir.add(bad_name, "bad:name?.txt", ps2hdd::pfs::kModeRegular);

    DirectorySector empty_dir(empty, root);
    DirectorySector nested_dir(nested, root);
    nested_dir.add(level1, "level1", ps2hdd::pfs::kModeDirectory);
    DirectorySector level1_dir(level1, nested);
    level1_dir.add(level2, "level2", ps2hdd::pfs::kModeDirectory);
    DirectorySector level2_dir(level2, level1);
    level2_dir.add(nested_file, "file.bin", ps2hdd::pfs::kModeRegular);

    const std::array one_root{root_data};
    const std::array one_empty{empty_data};
    const std::array one_nested{nested_data};
    const std::array one_level1{level1_data};
    const std::array one_level2{level2_data};
    write_object(image, volume_offset(main_base, root), make_inode(root, ps2hdd::pfs::kModeDirectory, kSectorSize, one_root));
    write_object(image, volume_offset(main_base, empty), make_inode(empty, ps2hdd::pfs::kModeDirectory, kSectorSize, one_empty));
    write_object(image, volume_offset(main_base, nested), make_inode(nested, ps2hdd::pfs::kModeDirectory, kSectorSize, one_nested));
    write_object(image, volume_offset(main_base, level1), make_inode(level1, ps2hdd::pfs::kModeDirectory, kSectorSize, one_level1));
    write_object(image, volume_offset(main_base, level2), make_inode(level2, ps2hdd::pfs::kModeDirectory, kSectorSize, one_level2));
    write_at(image, volume_offset(main_base, root_data), root_dir.finish());
    write_at(image, volume_offset(main_base, empty_data), empty_dir.finish());
    write_at(image, volume_offset(main_base, nested_data), nested_dir.finish());
    write_at(image, volume_offset(main_base, level1_data), level1_dir.finish());
    write_at(image, volume_offset(main_base, level2_data), level2_dir.finish());

    write_regular(image, main_base, sub_base, nested_file, nested_file_data, nested_bytes);
    write_regular(image, main_base, sub_base, small, small_data, small_bytes);
    write_regular(image, main_base, sub_base, boundary, boundary_data, boundary_bytes);
    write_regular(image, main_base, sub_base, con, con_data, con_bytes);
    write_regular(image, main_base, sub_base, foo, foo_data, foo_bytes);
    write_regular(image, main_base, sub_base, upper_foo, upper_foo_data, upper_foo_bytes);
    write_regular(image, main_base, sub_base, bad_name, bad_name_data, bad_name_bytes);

    const std::array fragmented_extents{fragmented_main, fragmented_sub};
    write_object(image, volume_offset(sub_base, fragmented),
                 make_inode(fragmented, ps2hdd::pfs::kModeRegular, fragmented_bytes.size(), fragmented_extents));
    write_at(image, volume_offset(main_base, fragmented_main), std::span(fragmented_bytes).first(kZoneSize));
    write_at(image, volume_offset(sub_base, fragmented_sub), std::span(fragmented_bytes).subspan(kZoneSize));

    ps2hdd::pfs::Inode segi{};
    segi.magic = ps2hdd::pfs::kSegdMagic;
    segi.inode_block = segi_inode;
    segi.last_segment = segi_meta;
    segi.next_segment = segi_meta;
    segi.data[0] = segi_inode;
    segi.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeRegular | 0x01B6);
    segi.size = segi_bytes.size();
    for (std::size_t i = 1; i < ps2hdd::pfs::kInodeMaxBlocks; ++i) {
        segi.data[i] = {static_cast<std::uint32_t>(1000U + i - 1U), 0, 1};
        write_at(image, volume_offset(main_base, segi.data[i]),
                 std::span(segi_bytes).subspan((i - 1U) * kZoneSize, kZoneSize));
    }
    segi.number_data = 116;
    segi.number_blocks = 116;
    segi.number_segdesg = 1;
    segi.checksum = ps2hdd::pfs::inode_checksum(segi);
    write_object(image, volume_offset(main_base, segi_inode), segi);

    ps2hdd::pfs::SegmentDescriptor indirect{};
    indirect.magic = ps2hdd::pfs::kSegiMagic;
    indirect.inode_block = segi_meta;
    indirect.last_segment = segi_meta;
    indirect.data[0] = segi_meta;
    indirect.data[1] = {1200,0,1};
    indirect.checksum = ps2hdd::pfs::segment_checksum(indirect);
    write_object(image, volume_offset(main_base, segi_meta), indirect);
    write_at(image, volume_offset(main_base, indirect.data[1]),
             std::span(segi_bytes).subspan(113U * kZoneSize, kZoneSize));

    image.close();
    check(static_cast<bool>(image), "close generated image");

    return {
        {std::filesystem::path("nested") / "level1" / "level2" / "file.bin", nested_bytes},
        {"small.txt", small_bytes},
        {"boundary.bin", boundary_bytes},
        {"fragmented.bin", fragmented_bytes},
        {"segi.bin", segi_bytes},
        {"_CON", con_bytes},
        {"foo", foo_bytes},
        {"FOO_2", upper_foo_bytes},
        {"bad_name_.txt", bad_name_bytes},
    };
}

std::vector<std::byte> read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(static_cast<bool>(input), "open exported file");
    const auto end = input.tellg();
    check(end >= 0, "read exported file size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        check(static_cast<bool>(input), "read exported file bytes");
    }
    return bytes;
}

std::string hex(std::span<const std::byte> digest)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned char>(byte);
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 0x0FU]);
    }
    return result;
}

void run_e2e()
{
    const std::array abc = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    check(hex(ps2hdd::test::sha256(abc)) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 helper matches the standard abc vector");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("ps2-driveforge-e2e-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{root};

    const auto image_path = root / "DriveForge-test.img";
    const auto export_path = root / "exported";
    const auto expected = generate_image(image_path);

    ps2hdd::FileBlockDevice device(image_path);
    check(device.is_open(), "production FileBlockDevice opens generated image");
    ps2hdd::apa::Reader apa_reader(device);
    const auto scan = apa_reader.scan();
    check(scan.ok(), "generated image passes production APA scan");
    check(scan.partitions.size() == 3, "generated image contains MBR/main/sub headers");

    const auto partition = std::find_if(scan.partitions.begin(), scan.partitions.end(), [](const auto& p) {
        return !p.is_sub() && p.id == "+TEST";
    });
    check(partition != scan.partitions.end(), "+TEST partition is discoverable");
    check(partition->sub_count == 1 && partition->sub_partitions.front().start == kSubLba,
          "+TEST exposes its real APA subpartition extent");

    ps2hdd::ApaVolume volume(device, *partition);
    ps2hdd::pfs::Reader reader(volume);
    check(reader.valid(), "generated PFS probes successfully");
    check(reader.probe_result().backup_matches, "generated PFS backup superblock matches");

    const auto exported = ps2hdd::pfs::export_to_host(reader, "/", export_path);
    check(exported.ok, std::string("recursive export failed: ") + exported.error);
    check(exported.stats.files == expected.size(), "recursive export file count matches fixture");
    check(exported.stats.directories == 5, "root/empty/nested directory count matches fixture");
    check(std::filesystem::is_directory(export_path / "EMPTY"), "empty PFS directory survives export");

    for (const auto& file : expected) {
        const auto actual = read_file(export_path / file.host_relative);
        check(actual.size() == file.contents.size(), "exported file size matches fixture");
        check(ps2hdd::test::sha256(actual) == ps2hdd::test::sha256(file.contents),
              "exported SHA-256 matches generated payload");
    }

    std::cout << "Generated-image E2E passed: " << exported.stats.files << " files, "
              << exported.stats.directories << " directories, " << exported.stats.bytes
              << " bytes verified by SHA-256.\n";
}

} // namespace

int main()
{
    try {
        run_e2e();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
