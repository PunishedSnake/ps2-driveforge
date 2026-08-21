#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_export.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
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

std::uint32_t rotr(std::uint32_t value, unsigned count)
{
    return std::rotr(value, static_cast<int>(count));
}

std::array<std::byte, 32> sha256(std::span<const std::byte> input)
{
    constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
    std::array<std::uint32_t, 8> h = {0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
                                      0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
    const std::size_t padded = ((input.size() + 9U + 63U) / 64U) * 64U;
    std::vector<std::byte> message(padded);
    std::copy(input.begin(), input.end(), message.begin());
    message[input.size()] = std::byte{0x80};
    for (unsigned i = 0; i < 8; ++i) {
        message[padded - 1U - i] = static_cast<std::byte>((bit_length >> (i * 8U)) & 0xFFU);
    }

    for (std::size_t chunk = 0; chunk < padded; chunk += 64) {
        std::array<std::uint32_t, 64> w{};
        for (unsigned i = 0; i < 16; ++i) {
            const auto* p = message.data() + static_cast<std::ptrdiff_t>(chunk + i * 4U);
            w[i] = (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) << 24U) |
                   (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 16U) |
                   (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 8U) |
                   static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3]));
        }
        for (unsigned i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto a=h[0], b=h[1], c=h[2], d=h[3], e=h[4], f=h[5], g=h[6], hh=h[7];
        for (unsigned i = 0; i < 64; ++i) {
            const auto s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto t1 = hh + s1 + ch + k[i] + w[i];
            const auto s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    std::array<std::byte, 32> digest{};
    for (unsigned i = 0; i < 8; ++i) {
        digest[i*4] = static_cast<std::byte>((h[i] >> 24U) & 0xFFU);
        digest[i*4+1] = static_cast<std::byte>((h[i] >> 16U) & 0xFFU);
        digest[i*4+2] = static_cast<std::byte>((h[i] >> 8U) & 0xFFU);
        digest[i*4+3] = static_cast<std::byte>(h[i] & 0xFFU);
    }
    return digest;
}

std::vector<std::byte> read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    check(static_cast<bool>(in), "open exported file");
    const auto size = in.tellg();
    check(size >= 0, "exported file size");
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    check(static_cast<bool>(in) || data.empty(), "read exported file");
    return data;
}

void write_at(std::fstream& image, std::uint64_t offset, std::span<const std::byte> bytes)
{
    image.seekp(static_cast<std::streamoff>(offset));
    check(static_cast<bool>(image), "seek test image");
    image.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(image), "write test image");
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

std::uint16_t dentry_size(std::string_view name)
{
    return static_cast<std::uint16_t>((8U + name.size() + 3U) & ~3U);
}

void put_dentry(std::span<std::byte> sector, std::size_t offset, ps2hdd::pfs::BlockInfo inode,
                std::string_view name, std::uint16_t mode, std::uint16_t allocated)
{
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset), &inode.number, sizeof(inode.number));
    sector[offset + 4] = static_cast<std::byte>(inode.subpart);
    sector[offset + 5] = static_cast<std::byte>(name.size());
    const std::uint16_t raw = static_cast<std::uint16_t>(mode | allocated);
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 6), &raw, sizeof(raw));
    if (!name.empty()) {
        std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 8), name.data(), name.size());
    }
}

struct EntrySpec {
    std::string name;
    ps2hdd::pfs::BlockInfo inode;
    std::uint16_t mode;
};

std::array<std::byte, kSectorSize> make_directory(ps2hdd::pfs::BlockInfo self,
                                                  ps2hdd::pfs::BlockInfo parent,
                                                  std::span<const EntrySpec> entries)
{
    std::array<std::byte, kSectorSize> sector{};
    std::size_t offset = 0;
    const auto append = [&](ps2hdd::pfs::BlockInfo inode, std::string_view name, std::uint16_t mode) mutable {
        const auto allocated = dentry_size(name);
        check(offset + allocated <= sector.size(), "directory fixture fits one sector");
        put_dentry(sector, offset, inode, name, mode, allocated);
        offset += allocated;
    };
    append(self, ".", ps2hdd::pfs::kModeDirectory);
    append(parent, "..", ps2hdd::pfs::kModeDirectory);
    for (const auto& entry : entries) {
        append(entry.inode, entry.name, entry.mode);
    }
    if (offset < sector.size()) {
        const auto remaining = static_cast<std::uint16_t>(sector.size() - offset);
        check((remaining & 3U) == 0 && remaining >= 8, "directory free entry alignment");
        put_dentry(sector, offset, {}, {}, 0, remaining);
    }
    return sector;
}

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
    check(extents.size() <= ps2hdd::pfs::kInodeMaxBlocks - 1, "direct fixture extent count");
    for (std::size_t i = 0; i < extents.size(); ++i) {
        inode.data[i + 1] = extents[i];
    }
    inode.number_data = static_cast<std::uint32_t>(1 + extents.size());
    inode.number_blocks = inode.number_data;
    inode.checksum = ps2hdd::pfs::inode_checksum(inode);
    return inode;
}

std::vector<std::byte> pattern(std::size_t size, unsigned seed)
{
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>((i * 37U + seed * 53U + (i >> 8U)) & 0xFFU);
    }
    return data;
}

struct ExpectedFile {
    std::filesystem::path host_path;
    std::vector<std::byte> data;
};

std::vector<ExpectedFile> build_image(const std::filesystem::path& image_path)
{
    std::fstream image(image_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    check(static_cast<bool>(image), "create test image");
    image.seekp(static_cast<std::streamoff>(kImageBytes - 1));
    image.put('\0');
    check(static_cast<bool>(image), "size test image");

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

    constexpr ps2hdd::pfs::BlockInfo root_loc{700, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo root_data{701, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo empty_loc{702, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo empty_data{703, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo nested_loc{704, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo nested_data{705, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo level1_loc{706, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo level1_data{707, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo level2_loc{708, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo level2_data{709, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo nested_file_loc{710, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo nested_file_data{711, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo small_loc{712, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo small_data{713, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo boundary_loc{714, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo boundary_data{715, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo fragmented_loc{100, 1, 1};
    constexpr ps2hdd::pfs::BlockInfo fragmented_main_data{716, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo fragmented_sub_data{110, 1, 1};
    constexpr ps2hdd::pfs::BlockInfo con_loc{717, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo con_data{718, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo foo_loc{719, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo foo_data{720, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo upper_foo_loc{721, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo upper_foo_data{722, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo bad_name_loc{723, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo bad_name_data{724, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo segi_loc{900, 0, 1};
    constexpr ps2hdd::pfs::BlockInfo segi_meta{901, 0, 1};

    ps2hdd::pfs::SuperBlock sb{};
    sb.magic = ps2hdd::pfs::kSuperMagic;
    sb.version = 3;
    sb.zone_size = kZoneSize;
    sb.num_subs = 1;
    sb.log = {650, 0, 1};
    sb.root = root_loc;
    std::array<std::byte, kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &sb, sizeof(sb));
    write_at(image, main_base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * kSectorSize, super_sector);
    write_at(image, main_base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperBackupSector) * kSectorSize, super_sector);

    const auto small = pattern(37, 1);
    const auto boundary = pattern(1024, 2);
    const auto fragmented = pattern(kZoneSize + 1337U, 3);
    const auto nested_file = pattern(333, 4);
    const auto con = pattern(5, 5);
    const auto foo = pattern(9, 6);
    const auto upper_foo = pattern(11, 7);
    const auto bad_name = pattern(13, 8);
    const auto segi_data = pattern(114U * kZoneSize, 9);

    const std::array root_entries = {
        EntrySpec{"EMPTY", empty_loc, ps2hdd::pfs::kModeDirectory},
        EntrySpec{"nested", nested_loc, ps2hdd::pfs::kModeDirectory},
        EntrySpec{"small.txt", small_loc, ps2hdd::pfs::kModeRegular},
        EntrySpec{"boundary.bin", boundary_loc, ps2hdd::pfs::kModeRegular},
        EntrySpec{"fragmented.bin", fragmented_loc, ps2hdd::pfs::kModeRegular},
        EntrySpec{"segi.bin", segi_loc, ps2hdd::pfs::kModeRegular},
        EntrySpec{"CON", con_loc, ps2hdd::pfs::kModeRegular},
        EntrySpec{"foo", foo_loc, ps2hdd::pfs::kModeRegular},
        EntrySpec{"FOO", upper_foo_loc, ps2hdd::pfs::kModeRegular},
        EntrySpec{"bad:name?.txt", bad_name_loc, ps2hdd::pfs::kModeRegular},
    };
    const std::array nested_entries = {EntrySpec{"level1", level1_loc, ps2hdd::pfs::kModeDirectory}};
    const std::array level1_entries = {EntrySpec{"level2", level2_loc, ps2hdd::pfs::kModeDirectory}};
    const std::array level2_entries = {EntrySpec{"file.bin", nested_file_loc, ps2hdd::pfs::kModeRegular}};

    write_object(image, main_base + static_cast<std::uint64_t>(root_loc.number) * kZoneSize,
                 make_inode(root_loc, ps2hdd::pfs::kModeDirectory, kSectorSize, std::array{root_data}));
    write_object(image, main_base + static_cast<std::uint64_t>(empty_loc.number) * kZoneSize,
                 make_inode(empty_loc, ps2hdd::pfs::kModeDirectory, kSectorSize, std::array{empty_data}));
    write_object(image, main_base + static_cast<std::uint64_t>(nested_loc.number) * kZoneSize,
                 make_inode(nested_loc, ps2hdd::pfs::kModeDirectory, kSectorSize, std::array{nested_data}));
    write_object(image, main_base + static_cast<std::uint64_t>(level1_loc.number) * kZoneSize,
                 make_inode(level1_loc, ps2hdd::pfs::kModeDirectory, kSectorSize, std::array{level1_data}));
    write_object(image, main_base + static_cast<std::uint64_t>(level2_loc.number) * kZoneSize,
                 make_inode(level2_loc, ps2hdd::pfs::kModeDirectory, kSectorSize, std::array{level2_data}));

    write_at(image, main_base + static_cast<std::uint64_t>(root_data.number) * kZoneSize,
             make_directory(root_loc, root_loc, root_entries));
    write_at(image, main_base + static_cast<std::uint64_t>(empty_data.number) * kZoneSize,
             make_directory(empty_loc, root_loc, {}));
    write_at(image, main_base + static_cast<std::uint64_t>(nested_data.number) * kZoneSize,
             make_directory(nested_loc, root_loc, nested_entries));
    write_at(image, main_base + static_cast<std::uint64_t>(level1_data.number) * kZoneSize,
             make_directory(level1_loc, nested_loc, level1_entries));
    write_at(image, main_base + static_cast<std::uint64_t>(level2_data.number) * kZoneSize,
             make_directory(level2_loc, level1_loc, level2_entries));

    const auto write_regular = [&](ps2hdd::pfs::BlockInfo inode_loc, ps2hdd::pfs::BlockInfo data_loc,
                                   const std::vector<std::byte>& data) {
        const auto base = inode_loc.subpart == 0 ? main_base : sub_base;
        write_object(image, base + static_cast<std::uint64_t>(inode_loc.number) * kZoneSize,
                     make_inode(inode_loc, ps2hdd::pfs::kModeRegular, data.size(), std::array{data_loc}));
        const auto data_base = data_loc.subpart == 0 ? main_base : sub_base;
        write_at(image, data_base + static_cast<std::uint64_t>(data_loc.number) * kZoneSize, data);
    };

    write_regular(nested_file_loc, nested_file_data, nested_file);
    write_regular(small_loc, small_data, small);
    write_regular(boundary_loc, boundary_data, boundary);
    write_regular(con_loc, con_data, con);
    write_regular(foo_loc, foo_data, foo);
    write_regular(upper_foo_loc, upper_foo_data, upper_foo);
    write_regular(bad_name_loc, bad_name_data, bad_name);

    const std::array fragmented_extents = {fragmented_main_data, fragmented_sub_data};
    write_object(image, sub_base + static_cast<std::uint64_t>(fragmented_loc.number) * kZoneSize,
                 make_inode(fragmented_loc, ps2hdd::pfs::kModeRegular, fragmented.size(), fragmented_extents));
    write_at(image, main_base + static_cast<std::uint64_t>(fragmented_main_data.number) * kZoneSize,
             std::span(fragmented).first(kZoneSize));
    write_at(image, sub_base + static_cast<std::uint64_t>(fragmented_sub_data.number) * kZoneSize,
             std::span(fragmented).subspan(kZoneSize));

    ps2hdd::pfs::Inode segi{};
    segi.magic = ps2hdd::pfs::kSegdMagic;
    segi.inode_block = segi_loc;
    segi.last_segment = segi_meta;
    segi.data[0] = segi_loc;
    segi.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeRegular | 0x01B6);
    segi.size = segi_data.size();
    for (std::size_t i = 1; i < ps2hdd::pfs::kInodeMaxBlocks; ++i) {
        segi.data[i] = {static_cast<std::uint32_t>(1000U + (i - 1U)), 0, 1};
        write_at(image, main_base + static_cast<std::uint64_t>(segi.data[i].number) * kZoneSize,
                 std::span(segi_data).subspan((i - 1U) * kZoneSize, kZoneSize));
    }
    segi.next_segment = segi_meta;
    segi.number_data = 116;
    segi.number_blocks = 116;
    segi.number_segdesg = 1;
    segi.checksum = ps2hdd::pfs::inode_checksum(segi);
    write_object(image, main_base + static_cast<std::uint64_t>(segi_loc.number) * kZoneSize, segi);

    ps2hdd::pfs::SegmentDescriptor indirect{};
    indirect.magic = ps2hdd::pfs::kSegiMagic;
    indirect.inode_block = segi_meta;
    indirect.last_segment = segi_meta;
    indirect.data[0] = segi_meta;
    indirect.data[1] = {1200, 0, 1};
    indirect.checksum = ps2hdd::pfs::segment_checksum(indirect);
    write_object(image, main_base + static_cast<std::uint64_t>(segi_meta.number) * kZoneSize, indirect);
    write_at(image, main_base + static_cast<std::uint64_t>(indirect.data[1].number) * kZoneSize,
             std::span(segi_data).subspan(113U * kZoneSize, kZoneSize));

    image.close();
    check(static_cast<bool>(image), "close test image");

    return {
        {"small.txt", small},
        {"boundary.bin", boundary},
        {"fragmented.bin", fragmented},
        {"segi.bin", segi_data},
        {"_CON", con},
        {"foo", foo},
        {"FOO_2", upper_foo},
        {"bad_name_.txt", bad_name},
        {std::filesystem::path("nested") / "level1" / "level2" / "file.bin", nested_file},
    };
}

void run_e2e()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = std::filesystem::temp_directory_path() /
                      ("ps2-driveforge-e2e-" + std::to_string(stamp));
    std::filesystem::create_directories(temp);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{temp};

    const auto image_path = temp / "DriveForge-test.img";
    const auto export_path = temp / "exported";
    const auto expected = build_image(image_path);

    ps2hdd::FileBlockDevice device(image_path);
    check(device.is_open(), "production FileBlockDevice opens generated image");
    ps2hdd::apa::Reader apa_reader(device);
    const auto scan = apa_reader.scan();
    check(scan.ok(), "generated image APA scan succeeds");
    check(scan.partitions.size() == 3, "generated image exposes MBR/main/sub APA headers");

    const auto it = std::find_if(scan.partitions.begin(), scan.partitions.end(), [](const auto& p) {
        return !p.is_sub() && p.id == "+TEST";
    });
    check(it != scan.partitions.end(), "+TEST main partition found");
    check(it->sub_count == 1 && it->sub_partitions[0].start == kSubLba, "APA subpartition attached to +TEST");

    ps2hdd::ApaVolume volume(device, *it);
    ps2hdd::pfs::Reader reader(volume);
    check(reader.valid(), "generated PFS probes successfully");
    check(reader.probe_result().backup_matches, "generated PFS primary/backup superblocks match");

    const auto result = ps2hdd::pfs::export_to_host(reader, "/", export_path);
    check(result.ok, std::string("recursive export succeeds: ") + result.error);
    check(result.stats.files == expected.size(), "exported file count matches fixture");
    check(result.stats.directories == 5, "root + empty + three nested directories exported");
    check(std::filesystem::is_directory(export_path / "EMPTY"), "empty directory preserved");

    for (const auto& file : expected) {
        const auto actual = read_file(export_path / file.host_path);
        check(actual.size() == file.data.size(), "exported size matches expected");
        check(sha256(actual) == sha256(file.data), "SHA-256 of exported file matches fixture payload");
    }

    std::cout << "E2E generated-image test passed: " << result.stats.files << " files, "
              << result.stats.directories << " directories, " << result.stats.bytes
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
