#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_filesystem.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class SparseDevice final : public ps2hdd::BlockDevice {
public:
    explicit SparseDevice(std::uint64_t size) : size_(size) {}
    std::uint64_t size_bytes() const override { return size_; }
    std::string display_name() const override { return "synthetic-pfs"; }
    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        const auto it = chunks_.find(offset);
        if (it == chunks_.end() || it->second.size() != out.size()) return false;
        std::memcpy(out.data(), it->second.data(), out.size());
        return true;
    }
    void put_bytes(std::uint64_t offset, std::span<const std::byte> bytes)
    {
        chunks_[offset] = std::vector<std::byte>(bytes.begin(), bytes.end());
    }
    void put_header(std::uint32_t lba, const ps2hdd::apa::Header& header)
    {
        put_bytes(static_cast<std::uint64_t>(lba) * ps2hdd::apa::kSectorSize,
                  std::as_bytes(std::span{&header, 1}));
    }
private:
    std::uint64_t size_;
    std::map<std::uint64_t, std::vector<std::byte>> chunks_;
};

ps2hdd::apa::Header make_header(const char* id, std::uint16_t type, std::uint32_t start,
                                std::uint32_t length, std::uint32_t prev, std::uint32_t next)
{
    ps2hdd::apa::Header header{};
    header.magic = ps2hdd::apa::kMagic;
    std::strncpy(header.id, id, sizeof(header.id) - 1);
    header.type = type;
    header.start = start;
    header.length = length;
    header.prev = prev;
    header.next = next;
    header.mbr.version = 2;
    header.checksum = ps2hdd::apa::checksum(header);
    return header;
}

constexpr std::uint32_t kPfsLba = 0x40000;
constexpr std::uint32_t kZoneSectors = 16;

std::uint64_t pfs_offset(std::uint32_t zone, std::uint32_t sector_in_zone = 0)
{
    return (static_cast<std::uint64_t>(kPfsLba) +
            static_cast<std::uint64_t>(zone) * kZoneSectors + sector_in_zone) *
           ps2hdd::apa::kSectorSize;
}

void put_inode_at(SparseDevice& dev, std::uint32_t partition_lba, std::uint32_t zone,
                  ps2hdd::pfs::Inode inode)
{
    inode.checksum = ps2hdd::pfs::inode_checksum(inode);
    const auto offset = (static_cast<std::uint64_t>(partition_lba) +
                         static_cast<std::uint64_t>(zone) * kZoneSectors) *
                        ps2hdd::apa::kSectorSize;
    dev.put_bytes(offset, std::as_bytes(std::span{&inode, 1}));
}

void put_inode(SparseDevice& dev, std::uint32_t zone, ps2hdd::pfs::Inode inode)
{
    put_inode_at(dev, kPfsLba, zone, inode);
}

void write_dentry(std::array<std::byte, ps2hdd::apa::kSectorSize>& sector, std::size_t offset,
                  std::uint32_t inode, std::uint8_t sub, std::string_view name,
                  std::uint16_t allocation, std::uint16_t type)
{
    ps2hdd::pfs::DirectoryEntryHeader header{};
    header.inode = inode;
    header.sub = sub;
    header.path_length = static_cast<std::uint8_t>(name.size());
    header.allocated_length = static_cast<std::uint16_t>(allocation | type);
    std::memcpy(sector.data() + offset, &header, sizeof(header));
    if (!name.empty()) {
        std::memcpy(sector.data() + offset + sizeof(header), name.data(), name.size());
    }
}

void setup_base(SparseDevice& dev, ps2hdd::pfs::SuperBlock& sb,
                std::uint32_t root_zone, std::uint32_t partition_sectors = 0x200000)
{
    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x40000, kPfsLba, kPfsLba);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    auto opl = make_header("+OPL", ps2hdd::apa::kTypePfs, kPfsLba, partition_sectors, 0, 0);
    dev.put_header(0, mbr);
    dev.put_header(kPfsLba, opl);

    sb = {};
    sb.magic = ps2hdd::pfs::kSuperMagic;
    sb.version = 3;
    sb.zone_size = 8192;
    sb.root.number = root_zone;
    sb.root.count = 1;
    sb.log.number = 50;
    sb.log.count = 16;

    std::array<std::byte, ps2hdd::apa::kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &sb, sizeof(sb));
    dev.put_bytes((static_cast<std::uint64_t>(kPfsLba) + ps2hdd::pfs::kSuperSector) *
                      ps2hdd::apa::kSectorSize,
                  super_sector);
    dev.put_bytes((static_cast<std::uint64_t>(kPfsLba) + ps2hdd::pfs::kSuperBackupSector) *
                      ps2hdd::apa::kSectorSize,
                  super_sector);
}

ps2hdd::pfs::Inode make_inode(std::uint32_t zone, std::uint16_t mode,
                              std::uint64_t size, std::uint32_t data_zone)
{
    ps2hdd::pfs::Inode inode{};
    inode.magic = ps2hdd::pfs::kSegdMagic;
    inode.inode_block = {zone, 0, 1};
    inode.last_segment = inode.inode_block;
    inode.data[0] = inode.inode_block;
    if (data_zone != 0) {
        inode.data[1] = {data_zone, 0, 1};
        inode.number_data = 2;
        inode.number_blocks = 2;
    } else {
        inode.number_data = 1;
        inode.number_blocks = 1;
    }
    inode.mode = mode;
    inode.size = size;
    inode.number_segdesg = 1;
    return inode;
}

void root_directory_and_nested_file_are_read()
{
    constexpr std::uint32_t root_zone = 600;
    constexpr std::uint32_t root_data = 601;
    constexpr std::uint32_t hello_zone = 602;
    constexpr std::uint32_t hello_data = 603;
    constexpr std::uint32_t cfg_zone = 604;
    constexpr std::uint32_t cfg_data = 605;
    constexpr std::uint32_t settings_zone = 606;
    constexpr std::uint32_t settings_data = 607;

    SparseDevice dev(8ULL * 1024 * 1024 * 1024);
    ps2hdd::pfs::SuperBlock sb{};
    setup_base(dev, sb, root_zone);

    auto root = make_inode(root_zone, 0x11FF, 512, root_data);
    auto hello = make_inode(hello_zone, 0x21FF, 11, hello_data);
    auto cfg = make_inode(cfg_zone, 0x11FF, 512, cfg_data);
    auto settings = make_inode(settings_zone, 0x21FF, 7, settings_data);
    put_inode(dev, root_zone, root);
    put_inode(dev, hello_zone, hello);
    put_inode(dev, cfg_zone, cfg);
    put_inode(dev, settings_zone, settings);

    std::array<std::byte, 512> root_sector{};
    write_dentry(root_sector, 0, root_zone, 0, ".", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(root_sector, 12, root_zone, 0, "..", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(root_sector, 24, cfg_zone, 0, "CFG", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(root_sector, 36, hello_zone, 0, "hello.txt", 20, ps2hdd::pfs::kModeRegular);
    write_dentry(root_sector, 56, 0, 0, "", 456, 0);
    dev.put_bytes(pfs_offset(root_data), root_sector);

    std::array<std::byte, 512> cfg_sector{};
    write_dentry(cfg_sector, 0, cfg_zone, 0, ".", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(cfg_sector, 12, root_zone, 0, "..", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(cfg_sector, 24, settings_zone, 0, "settings.cfg", 20, ps2hdd::pfs::kModeRegular);
    write_dentry(cfg_sector, 44, 0, 0, "", 468, 0);
    dev.put_bytes(pfs_offset(cfg_data), cfg_sector);

    std::array<std::byte, 512> hello_sector{};
    std::memcpy(hello_sector.data(), "hello world", 11);
    dev.put_bytes(pfs_offset(hello_data), hello_sector);
    std::array<std::byte, 512> settings_sector{};
    std::memcpy(settings_sector.data(), "bocchi!", 7);
    dev.put_bytes(pfs_offset(settings_data), settings_sector);

    ps2hdd::apa::Reader apa_reader(dev);
    const auto apa = apa_reader.scan();
    check(apa.ok(), "APA scan");
    ps2hdd::ApaVolume volume(dev, apa.partitions[1]);
    ps2hdd::pfs::FileSystem fs(volume);
    check(fs.mount(), "mount root fixture");

    const auto listing = fs.list_directory(fs.superblock().root);
    check(listing.ok(), "root listing");
    check(listing.entries.size() == 2, "root visible entry count");
    check(listing.entries[0].name == "CFG", "root CFG name");
    check(listing.entries[0].is_directory(), "root CFG type");

    std::string error;
    const auto node = fs.resolve("/CFG/settings.cfg", &error);
    check(node.has_value(), "resolve nested settings.cfg");
    check(node->inode.size == 7, "nested file size");

    std::array<std::byte, 7> data{};
    std::size_t bytes_read = 0;
    check(fs.read_file(*node, 0, data, bytes_read, &error), "read nested settings.cfg");
    check(bytes_read == 7, "nested bytes read");
    check(std::memcmp(data.data(), "bocchi!", 7) == 0, "nested file contents");
}

void directory_can_span_multiple_sectors()
{
    constexpr std::uint32_t root_zone = 620;
    constexpr std::uint32_t root_data = 621;
    constexpr std::uint32_t file_a_zone = 622;
    constexpr std::uint32_t file_b_zone = 623;

    SparseDevice dev(8ULL * 1024 * 1024 * 1024);
    ps2hdd::pfs::SuperBlock sb{};
    setup_base(dev, sb, root_zone);

    auto root = make_inode(root_zone, 0x11FF, 1024, root_data);
    put_inode(dev, root_zone, root);
    put_inode(dev, file_a_zone, make_inode(file_a_zone, 0x21FF, 0, 0));
    put_inode(dev, file_b_zone, make_inode(file_b_zone, 0x21FF, 0, 0));

    std::array<std::byte, 512> first{};
    write_dentry(first, 0, root_zone, 0, ".", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(first, 12, root_zone, 0, "..", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(first, 24, file_a_zone, 0, "first.bin", 488, ps2hdd::pfs::kModeRegular);
    dev.put_bytes(pfs_offset(root_data), first);

    std::array<std::byte, 512> second{};
    write_dentry(second, 0, file_b_zone, 0, "second.bin", 512, ps2hdd::pfs::kModeRegular);
    dev.put_bytes(pfs_offset(root_data, 1), second);

    ps2hdd::apa::Reader apa_reader(dev);
    const auto apa = apa_reader.scan();
    ps2hdd::ApaVolume volume(dev, apa.partitions[1]);
    ps2hdd::pfs::FileSystem fs(volume);
    check(fs.mount(), "mount multi-sector directory fixture");
    const auto listing = fs.list_directory(fs.superblock().root);
    check(listing.ok(), "multi-sector listing");
    check(listing.entries.size() == 2, "multi-sector visible entry count");
    check(listing.entries[1].name == "second.bin", "second sector entry name");
}

void indirect_segi_extent_is_read()
{
    constexpr std::uint32_t root_zone = 640;
    constexpr std::uint32_t root_data = 641;
    constexpr std::uint32_t file_zone = 642;
    constexpr std::uint32_t segi_zone = 700;
    constexpr std::uint32_t indirect_data_zone = 900;

    SparseDevice dev(8ULL * 1024 * 1024 * 1024);
    ps2hdd::pfs::SuperBlock sb{};
    setup_base(dev, sb, root_zone);
    put_inode(dev, root_zone, make_inode(root_zone, 0x11FF, 512, root_data));

    std::array<std::byte, 512> root_sector{};
    write_dentry(root_sector, 0, root_zone, 0, ".", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(root_sector, 12, root_zone, 0, "..", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(root_sector, 24, file_zone, 0, "large.bin", 488, ps2hdd::pfs::kModeRegular);
    dev.put_bytes(pfs_offset(root_data), root_sector);

    ps2hdd::pfs::Inode file{};
    file.magic = ps2hdd::pfs::kSegdMagic;
    file.inode_block = {file_zone, 0, 1};
    file.data[0] = file.inode_block;
    for (std::size_t i = 1; i < ps2hdd::pfs::kDirectBlockInfos; ++i) {
        file.data[i] = {static_cast<std::uint32_t>(1000 + i), 0, 1};
    }
    file.next_segment = {segi_zone, 0, 1};
    file.last_segment = file.next_segment;
    file.mode = 0x21FF;
    file.size = 114ULL * 8192ULL;
    file.number_data = 116; // SEGD + 113 direct extents + SEGI + 1 indirect extent.
    file.number_blocks = 116;
    file.number_segdesg = 2;
    put_inode(dev, file_zone, file);

    ps2hdd::pfs::Inode segi{};
    segi.magic = ps2hdd::pfs::kSegiMagic;
    segi.inode_block = file.inode_block;
    segi.data[0] = {segi_zone, 0, 1};
    segi.data[1] = {indirect_data_zone, 0, 1};
    put_inode(dev, segi_zone, segi);

    std::array<std::byte, 512> payload{};
    std::memcpy(payload.data(), "INDIRECT", 8);
    dev.put_bytes(pfs_offset(indirect_data_zone), payload);

    ps2hdd::apa::Reader apa_reader(dev);
    const auto apa = apa_reader.scan();
    ps2hdd::ApaVolume volume(dev, apa.partitions[1]);
    ps2hdd::pfs::FileSystem fs(volume);
    check(fs.mount(), "mount SEGI fixture");

    std::string error;
    const auto node = fs.resolve("/large.bin", &error);
    check(node.has_value(), "resolve indirect file");
    std::array<std::byte, 8> data{};
    std::size_t bytes_read = 0;
    check(fs.read_file(*node, 113ULL * 8192ULL, data, bytes_read, &error), "read SEGI extent");
    check(bytes_read == 8, "SEGI bytes read");
    check(std::memcmp(data.data(), "INDIRECT", 8) == 0, "SEGI payload");
}

void pfs_subpartition_is_followed()
{
    constexpr std::uint32_t sub_lba = 0x240000;
    constexpr std::uint32_t main_length = 0x180000;
    constexpr std::uint32_t sub_length = 0x80000;
    constexpr std::uint32_t root_zone = 660;
    constexpr std::uint32_t root_data = 661;
    constexpr std::uint32_t file_zone = 20;
    constexpr std::uint32_t file_data = 21;

    SparseDevice dev(8ULL * 1024 * 1024 * 1024);

    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x40000, sub_lba, kPfsLba);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr);

    auto main = make_header("+BIGPFS", ps2hdd::apa::kTypePfs, kPfsLba, main_length, 0, sub_lba);
    main.nsub = 1;
    main.subs[0].start = sub_lba;
    main.subs[0].length = sub_length;
    main.checksum = ps2hdd::apa::checksum(main);

    auto sub = make_header("", ps2hdd::apa::kTypePfs, sub_lba, sub_length, kPfsLba, 0);
    sub.flags = ps2hdd::apa::kFlagSub;
    sub.main = kPfsLba;
    sub.number = 0;
    sub.checksum = ps2hdd::apa::checksum(sub);

    dev.put_header(0, mbr);
    dev.put_header(kPfsLba, main);
    dev.put_header(sub_lba, sub);

    ps2hdd::pfs::SuperBlock sb{};
    sb.magic = ps2hdd::pfs::kSuperMagic;
    sb.version = 3;
    sb.zone_size = 8192;
    sb.num_subs = 1;
    sb.root = {root_zone, 0, 1};
    sb.log = {50, 0, 16};

    std::array<std::byte, 512> super_sector{};
    std::memcpy(super_sector.data(), &sb, sizeof(sb));
    dev.put_bytes((static_cast<std::uint64_t>(kPfsLba) + ps2hdd::pfs::kSuperSector) * 512,
                  super_sector);
    dev.put_bytes((static_cast<std::uint64_t>(kPfsLba) + ps2hdd::pfs::kSuperBackupSector) * 512,
                  super_sector);

    auto root = make_inode(root_zone, 0x11FF, 512, root_data);
    put_inode(dev, root_zone, root);

    ps2hdd::pfs::Inode file{};
    file.magic = ps2hdd::pfs::kSegdMagic;
    file.inode_block = {file_zone, 1, 1};
    file.last_segment = file.inode_block;
    file.data[0] = file.inode_block;
    file.data[1] = {file_data, 1, 1};
    file.mode = 0x21FF;
    file.size = 8;
    file.number_blocks = 2;
    file.number_data = 2;
    file.number_segdesg = 1;
    put_inode_at(dev, sub_lba, file_zone, file);

    std::array<std::byte, 512> root_sector{};
    write_dentry(root_sector, 0, root_zone, 0, ".", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(root_sector, 12, root_zone, 0, "..", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(root_sector, 24, file_zone, 1, "sub.bin", 488, ps2hdd::pfs::kModeRegular);
    dev.put_bytes(pfs_offset(root_data), root_sector);

    std::array<std::byte, 512> payload{};
    std::memcpy(payload.data(), "SUBPART!", 8);
    dev.put_bytes((static_cast<std::uint64_t>(sub_lba) + file_data * 16ULL) * 512, payload);

    ps2hdd::apa::Reader apa_reader(dev);
    const auto apa = apa_reader.scan();
    check(apa.ok(), "APA scan with PFS subpartition");
    check(apa.partitions.size() == 3, "APA subpartition fixture partition count");
    check(apa.partitions[1].sub_partitions.size() == 1, "APA PFS subpartition attached");

    ps2hdd::ApaVolume volume(dev, apa.partitions[1]);
    ps2hdd::pfs::FileSystem fs(volume);
    check(fs.mount(), "mount PFS with APA subpartition");

    std::string error;
    const auto node = fs.resolve("/sub.bin", &error);
    check(node.has_value(), "resolve inode stored in PFS subpartition");
    check(node->location.subpart == 1, "resolved PFS subpartition index");

    std::array<std::byte, 8> data{};
    std::size_t bytes_read = 0;
    check(fs.read_file(*node, 0, data, bytes_read, &error), "read data stored in PFS subpartition");
    check(bytes_read == 8, "PFS subpartition bytes read");
    check(std::memcmp(data.data(), "SUBPART!", 8) == 0, "PFS subpartition payload");
}

} // namespace

int main()
{
    try {
        root_directory_and_nested_file_are_read();
        directory_can_span_multiple_sectors();
        indirect_segi_extent_is_read();
        pfs_subpartition_is_followed();
        std::cout << "All PFS filesystem tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
