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
    std::string display_name() const override { return "synthetic-segi"; }

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

void indirect_descriptor_is_followed()
{
    constexpr std::uint32_t pfs_lba = 0x40000;
    constexpr std::uint32_t zone_size = 8192;
    constexpr std::uint32_t inode_zone = 100;
    constexpr std::uint32_t segi_zone = 500;
    constexpr std::uint32_t target_zone = 600;

    SparseDevice dev(8ULL * 1024ULL * 1024ULL * 1024ULL);

    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x40000, pfs_lba, pfs_lba);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    const auto pfs_part = make_header("+BIG", ps2hdd::apa::kTypePfs, pfs_lba, 0x200000, 0, 0);
    dev.put_object(0, mbr);
    dev.put_object(static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize, pfs_part);

    ps2hdd::pfs::SuperBlock sb{};
    sb.magic = ps2hdd::pfs::kSuperMagic;
    sb.version = 3;
    sb.zone_size = zone_size;
    sb.root = {inode_zone, 0, 1};
    sb.log = {50, 0, 1};
    std::array<std::byte, ps2hdd::apa::kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &sb, sizeof(sb));
    const auto partition_base = static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize;
    dev.put(partition_base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize,
            super_sector);
    dev.put(partition_base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperBackupSector) * ps2hdd::apa::kSectorSize,
            super_sector);

    ps2hdd::pfs::Inode inode{};
    inode.magic = ps2hdd::pfs::kSegdMagic;
    inode.inode_block = {inode_zone, 0, 1};
    inode.last_segment = {segi_zone, 0, 1};
    inode.next_segment = {segi_zone, 0, 1};
    inode.data[0] = inode.inode_block;
    for (std::size_t i = 1; i < ps2hdd::pfs::kInodeMaxBlocks; ++i) {
        inode.data[i] = {static_cast<std::uint32_t>(200 + i), 0, 1};
    }
    inode.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeRegular | 0x01B6);
    inode.number_data = 116; // primary self + 113 data + SEGI self + one SEGI data block
    inode.number_blocks = 115;
    inode.number_segdesg = 1;
    inode.size = 114ULL * zone_size;
    inode.checksum = ps2hdd::pfs::inode_checksum(inode);

    ps2hdd::pfs::SegmentDescriptor segi{};
    segi.magic = ps2hdd::pfs::kSegiMagic;
    segi.inode_block = inode.inode_block;
    segi.last_segment = inode.inode_block;
    segi.data[0] = {segi_zone, 0, 1};
    segi.data[1] = {target_zone, 0, 1};
    segi.checksum = ps2hdd::pfs::segment_checksum(segi);

    dev.put_object(partition_base + static_cast<std::uint64_t>(inode_zone) * zone_size, inode);
    dev.put_object(partition_base + static_cast<std::uint64_t>(segi_zone) * zone_size, segi);

    std::array<std::byte, ps2hdd::apa::kSectorSize> sector0{};
    std::array<std::byte, ps2hdd::apa::kSectorSize> sector1{};
    for (std::size_t i = 0; i < sector0.size(); ++i) {
        sector0[i] = static_cast<std::byte>((i * 3U) & 0xFFU);
        sector1[i] = static_cast<std::byte>((i * 5U + 7U) & 0xFFU);
    }
    const auto target_base = partition_base + static_cast<std::uint64_t>(target_zone) * zone_size;
    dev.put(target_base, sector0);
    dev.put(target_base + ps2hdd::apa::kSectorSize, sector1);

    ps2hdd::apa::Reader apa_reader(dev);
    const auto scan = apa_reader.scan();
    check(scan.ok() && scan.partitions.size() == 2, "APA scan for SEGI test");

    ps2hdd::ApaVolume volume(dev, scan.partitions[1]);
    ps2hdd::pfs::Reader pfs(volume);
    check(pfs.valid(), "PFS SEGI reader valid");
    const auto node = pfs.read_inode({inode_zone, 0, 1});
    check(node.has_value(), "large inode readable");

    const std::uint64_t offset = 113ULL * zone_size + 500;
    std::array<std::byte, 40> data{};
    check(pfs.read(*node, offset, data), "SEGI range read succeeds");
    for (std::size_t i = 0; i < 12; ++i) {
        check(data[i] == sector0[500 + i], "SEGI bytes before sector boundary match");
    }
    for (std::size_t i = 12; i < data.size(); ++i) {
        check(data[i] == sector1[i - 12], "SEGI bytes after sector boundary match");
    }
}

} // namespace

int main()
{
    try {
        indirect_descriptor_is_followed();
        std::cout << "PFS SEGI tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
