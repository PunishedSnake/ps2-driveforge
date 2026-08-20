#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
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
    std::string display_name() const override { return "synthetic-pfs-file"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        const auto it = chunks_.find(offset);
        if (it == chunks_.end() || it->second.size() != out.size()) {
            return false;
        }
        std::memcpy(out.data(), it->second.data(), out.size());
        return true;
    }

    void put(std::uint64_t offset, std::span<const std::byte> bytes)
    {
        chunks_[offset] = std::vector<std::byte>(bytes.begin(), bytes.end());
    }

    template <typename T>
    void put_object(std::uint64_t offset, const T& value)
    {
        put(offset, std::as_bytes(std::span{&value, 1}));
    }

private:
    std::uint64_t size_;
    std::map<std::uint64_t, std::vector<std::byte>> chunks_;
};

ps2hdd::apa::Header make_header(const char* id, std::uint16_t type, std::uint32_t start,
                                std::uint32_t length, std::uint32_t prev, std::uint32_t next)
{
    ps2hdd::apa::Header h{};
    h.magic = ps2hdd::apa::kMagic;
    std::strncpy(h.id, id, sizeof(h.id) - 1);
    h.type = type;
    h.start = start;
    h.length = length;
    h.prev = prev;
    h.next = next;
    h.mbr.version = 2;
    h.checksum = ps2hdd::apa::checksum(h);
    return h;
}

void put_dentry(std::span<std::byte> sector, std::size_t offset, std::uint32_t inode,
                std::string_view name, std::uint16_t mode, std::uint16_t allocated)
{
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset), &inode, sizeof(inode));
    sector[offset + 4] = std::byte{0};
    sector[offset + 5] = static_cast<std::byte>(name.size());
    const std::uint16_t raw = static_cast<std::uint16_t>(mode | allocated);
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 6), &raw, sizeof(raw));
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 8), name.data(), name.size());
}

void cross_sector_file_read()
{
    constexpr std::uint32_t pfs_lba = 0x40000;
    constexpr std::uint32_t zone_size = 8192;
    constexpr std::uint32_t root_zone = 100;
    constexpr std::uint32_t root_data_zone = 101;
    constexpr std::uint32_t file_inode_zone = 102;
    constexpr std::uint32_t file_data_zone = 103;

    SparseDevice dev(8ULL * 1024ULL * 1024ULL * 1024ULL);

    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x40000, pfs_lba, pfs_lba);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    const auto opl = make_header("+OPL", ps2hdd::apa::kTypePfs, pfs_lba, 0x100000, 0, 0);
    dev.put_object(0, mbr);
    dev.put_object(static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize, opl);

    ps2hdd::pfs::SuperBlock sb{};
    sb.magic = ps2hdd::pfs::kSuperMagic;
    sb.version = 3;
    sb.zone_size = zone_size;
    sb.root = {root_zone, 0, 1};
    sb.log = {50, 0, 1};
    std::array<std::byte, ps2hdd::apa::kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &sb, sizeof(sb));
    const auto partition_base = static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize;
    dev.put(partition_base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize,
            super_sector);
    dev.put(partition_base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperBackupSector) * ps2hdd::apa::kSectorSize,
            super_sector);

    ps2hdd::pfs::Inode root{};
    root.magic = ps2hdd::pfs::kSegdMagic;
    root.inode_block = {root_zone, 0, 1};
    root.last_segment = root.inode_block;
    root.data[0] = root.inode_block;
    root.data[1] = {root_data_zone, 0, 1};
    root.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 0x01FF);
    root.size = 40;
    root.number_data = 2;
    root.number_blocks = 2;
    root.checksum = ps2hdd::pfs::inode_checksum(root);

    ps2hdd::pfs::Inode file{};
    file.magic = ps2hdd::pfs::kSegdMagic;
    file.inode_block = {file_inode_zone, 0, 1};
    file.last_segment = file.inode_block;
    file.data[0] = file.inode_block;
    file.data[1] = {file_data_zone, 0, 1};
    file.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeRegular | 0x01B6);
    file.size = 1024;
    file.number_data = 2;
    file.number_blocks = 2;
    file.checksum = ps2hdd::pfs::inode_checksum(file);

    dev.put_object(partition_base + static_cast<std::uint64_t>(root_zone) * zone_size, root);
    dev.put_object(partition_base + static_cast<std::uint64_t>(file_inode_zone) * zone_size, file);

    std::array<std::byte, ps2hdd::apa::kSectorSize> directory{};
    put_dentry(directory, 0, root_zone, ".", ps2hdd::pfs::kModeDirectory, 12);
    put_dentry(directory, 12, root_zone, "..", ps2hdd::pfs::kModeDirectory, 12);
    put_dentry(directory, 24, file_inode_zone, "TEST.TXT", ps2hdd::pfs::kModeRegular, 16);
    dev.put(partition_base + static_cast<std::uint64_t>(root_data_zone) * zone_size, directory);

    std::array<std::byte, ps2hdd::apa::kSectorSize> sector0{};
    std::array<std::byte, ps2hdd::apa::kSectorSize> sector1{};
    for (std::size_t i = 0; i < sector0.size(); ++i) {
        sector0[i] = static_cast<std::byte>(i & 0xFFU);
        sector1[i] = static_cast<std::byte>((i + 17U) & 0xFFU);
    }
    const auto file_base = partition_base + static_cast<std::uint64_t>(file_data_zone) * zone_size;
    dev.put(file_base, sector0);
    dev.put(file_base + ps2hdd::apa::kSectorSize, sector1);

    ps2hdd::apa::Reader apa_reader(dev);
    const auto scan = apa_reader.scan();
    check(scan.ok() && scan.partitions.size() == 2, "APA scan for file test");

    ps2hdd::ApaVolume volume(dev, scan.partitions[1]);
    ps2hdd::pfs::Reader pfs(volume);
    check(pfs.valid(), "PFS file reader valid");
    const auto node = pfs.resolve("TEST.TXT");
    check(node.has_value(), "TEST.TXT resolves");

    std::array<std::byte, 40> data{};
    check(pfs.read(*node, 500, data), "cross-sector read succeeds");
    for (std::size_t i = 0; i < 12; ++i) {
        check(data[i] == sector0[500 + i], "bytes before sector boundary match");
    }
    for (std::size_t i = 12; i < data.size(); ++i) {
        check(data[i] == sector1[i - 12], "bytes after sector boundary match");
    }
}

} // namespace

int main()
{
    try {
        cross_sector_file_read();
        std::cout << "PFS file read tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
