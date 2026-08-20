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
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class SparseDevice final : public ps2hdd::BlockDevice {
public:
    explicit SparseDevice(std::uint64_t size) : size_(size) {}

    std::uint64_t size_bytes() const override { return size_; }
    std::string display_name() const override { return "synthetic-pfs"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        const auto it = chunks_.find(offset);
        if (it == chunks_.end() || it->second.size() != out.size()) {
            return false;
        }
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

void put_inode(SparseDevice& dev, std::uint32_t partition_lba, std::uint32_t zone,
               ps2hdd::pfs::Inode inode)
{
    inode.checksum = ps2hdd::pfs::inode_checksum(inode);
    const auto offset = (static_cast<std::uint64_t>(partition_lba) +
                         static_cast<std::uint64_t>(zone) * 16U) * ps2hdd::apa::kSectorSize;
    dev.put_bytes(offset, std::as_bytes(std::span{&inode, 1}));
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

void root_directory_and_file_are_read()
{
    constexpr std::uint32_t pfs_lba = 0x40000;
    constexpr std::uint32_t root_zone = 600;
    constexpr std::uint32_t root_data_zone = 601;
    constexpr std::uint32_t file_zone = 602;
    constexpr std::uint32_t file_data_zone = 603;

    SparseDevice dev(8ULL * 1024 * 1024 * 1024);
    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x40000, pfs_lba, pfs_lba);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    auto opl = make_header("+OPL", ps2hdd::apa::kTypePfs, pfs_lba, 0x100000, 0, 0);
    dev.put_header(0, mbr);
    dev.put_header(pfs_lba, opl);

    ps2hdd::pfs::SuperBlock sb{};
    sb.magic = ps2hdd::pfs::kSuperMagic;
    sb.version = 3;
    sb.zone_size = 8192;
    sb.root.number = root_zone;
    sb.root.count = 1;
    sb.log.number = 50;
    sb.log.count = 16;

    std::array<std::byte, ps2hdd::apa::kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &sb, sizeof(sb));
    dev.put_bytes((static_cast<std::uint64_t>(pfs_lba) + ps2hdd::pfs::kSuperSector) *
                      ps2hdd::apa::kSectorSize,
                  super_sector);
    dev.put_bytes((static_cast<std::uint64_t>(pfs_lba) + ps2hdd::pfs::kSuperBackupSector) *
                      ps2hdd::apa::kSectorSize,
                  super_sector);

    ps2hdd::pfs::Inode root{};
    root.magic = ps2hdd::pfs::kSegdMagic;
    root.inode_block = sb.root;
    root.last_segment = sb.root;
    root.data[0] = sb.root;
    root.data[1] = {root_data_zone, 0, 1};
    root.mode = 0x11FF;
    root.size = 512;
    root.number_blocks = 2;
    root.number_data = 2;
    root.number_segdesg = 1;
    put_inode(dev, pfs_lba, root_zone, root);

    ps2hdd::pfs::Inode file{};
    file.magic = ps2hdd::pfs::kSegdMagic;
    file.inode_block = {file_zone, 0, 1};
    file.last_segment = file.inode_block;
    file.data[0] = file.inode_block;
    file.data[1] = {file_data_zone, 0, 1};
    file.mode = 0x21FF;
    file.size = 11;
    file.number_blocks = 2;
    file.number_data = 2;
    file.number_segdesg = 1;
    put_inode(dev, pfs_lba, file_zone, file);

    std::array<std::byte, ps2hdd::apa::kSectorSize> root_sector{};
    write_dentry(root_sector, 0, root_zone, 0, ".", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(root_sector, 12, root_zone, 0, "..", 12, ps2hdd::pfs::kModeDirectory);
    write_dentry(root_sector, 24, file_zone, 0, "hello.txt", 20, ps2hdd::pfs::kModeRegular);
    write_dentry(root_sector, 44, 0, 0, "", 468, 0);
    dev.put_bytes((static_cast<std::uint64_t>(pfs_lba) + root_data_zone * 16ULL) *
                      ps2hdd::apa::kSectorSize,
                  root_sector);

    std::array<std::byte, ps2hdd::apa::kSectorSize> file_sector{};
    std::memcpy(file_sector.data(), "hello world", 11);
    dev.put_bytes((static_cast<std::uint64_t>(pfs_lba) + file_data_zone * 16ULL) *
                      ps2hdd::apa::kSectorSize,
                  file_sector);

    ps2hdd::apa::Reader apa_reader(dev);
    const auto apa_result = apa_reader.scan();
    check(apa_result.ok(), "APA scan for PFS filesystem test");

    ps2hdd::ApaVolume volume(dev, apa_result.partitions[1]);
    ps2hdd::pfs::FileSystem fs(volume);
    check(fs.mount(), "PFS filesystem mount");

    const auto listing = fs.list_directory(fs.superblock().root);
    check(listing.ok(), "PFS root directory listing");
    check(listing.entries.size() == 1, "PFS root visible entry count");
    check(listing.entries[0].name == "hello.txt", "PFS root entry name");
    check(listing.entries[0].is_regular(), "PFS root entry type");

    std::string error;
    const auto node = fs.resolve("/hello.txt", &error);
    check(node.has_value(), "PFS resolve /hello.txt");
    check(node->inode.size == 11, "PFS resolved file size");

    std::array<std::byte, 5> data{};
    std::size_t bytes_read = 0;
    check(fs.read_file(*node, 6, data, bytes_read, &error), "PFS partial file read");
    check(bytes_read == 5, "PFS partial file byte count");
    check(std::memcmp(data.data(), "world", 5) == 0, "PFS partial file contents");
}

} // namespace

int main()
{
    try {
        root_directory_and_file_are_read();
        std::cout << "All PFS filesystem tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
