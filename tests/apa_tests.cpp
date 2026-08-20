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

using ps2hdd::apa::Header;

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
    std::string display_name() const override { return "synthetic"; }

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

    template <typename T>
    void put_object(std::uint64_t offset, const T& object)
    {
        put_bytes(offset, std::as_bytes(std::span{&object, 1}));
    }

    void put_header(std::uint32_t lba, const Header& h)
    {
        put_object(static_cast<std::uint64_t>(lba) * ps2hdd::apa::kSectorSize, h);
    }

private:
    std::uint64_t size_;
    std::map<std::uint64_t, std::vector<std::byte>> chunks_;
};

Header make_header(const char* id, std::uint16_t type, std::uint32_t start,
                   std::uint32_t length, std::uint32_t prev, std::uint32_t next)
{
    Header h{};
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
                std::uint8_t sub, std::string_view name, std::uint16_t mode,
                std::uint16_t allocated)
{
    check(offset + allocated <= sector.size(), "dentry fits");
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset), &inode, sizeof(inode));
    sector[offset + 4] = static_cast<std::byte>(sub);
    sector[offset + 5] = static_cast<std::byte>(name.size());
    const std::uint16_t raw_len = static_cast<std::uint16_t>(mode | allocated);
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 6), &raw_len, sizeof(raw_len));
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 8), name.data(), name.size());
}

void valid_chain_is_read()
{
    constexpr std::uint32_t pfs_lba = 0x40000;
    SparseDevice dev(8ULL * 1024 * 1024 * 1024);

    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x40000, pfs_lba, pfs_lba);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr);

    auto opl = make_header("+OPL", ps2hdd::apa::kTypePfs, pfs_lba, 0x80000, 0, 0);

    dev.put_header(0, mbr);
    dev.put_header(pfs_lba, opl);

    ps2hdd::apa::Reader reader(dev);
    const auto result = reader.scan();

    check(result.ok(), "result.ok()");
    check(result.mbr_valid, "result.mbr_valid");
    check(result.apa_version == 2, "result.apa_version == 2");
    check(result.partitions.size() == 2, "result.partitions.size() == 2");
    check(result.partitions[1].id == "+OPL", "result.partitions[1].id == '+OPL'");
    check(result.partitions[1].type == ps2hdd::apa::kTypePfs, "result.partitions[1].type == ps2hdd::apa::kTypePfs");
}

void bad_checksum_is_rejected()
{
    SparseDevice dev(1024 * 1024);
    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x40000, 0, 0);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr) + 1;
    dev.put_header(0, mbr);

    ps2hdd::apa::Reader reader(dev);
    const auto result = reader.scan();
    check(!result.ok(), "!result.ok()");
    check(!result.mbr_valid, "!result.mbr_valid");
    check(!result.issues.empty(), "!result.issues.empty()");
}

void cycle_is_detected()
{
    constexpr std::uint32_t pfs_lba = 0x40000;
    SparseDevice dev(8ULL * 1024 * 1024 * 1024);

    auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x40000, pfs_lba, pfs_lba);
    std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    auto pfs = make_header("+BROKEN", ps2hdd::apa::kTypePfs, pfs_lba, 0x40000, 0, pfs_lba);

    dev.put_header(0, mbr);
    dev.put_header(pfs_lba, pfs);

    ps2hdd::apa::Reader reader(dev);
    const auto result = reader.scan();
    check(!result.ok(), "!result.ok()");
    check(result.partitions.size() == 2, "result.partitions.size() == 2");
}

void pfs_superblock_is_probed()
{
    constexpr std::uint32_t pfs_lba = 0x40000;
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
    sb.num_subs = 0;
    sb.root.number = 100;
    sb.root.count = 1;
    sb.log.number = 50;
    sb.log.count = 16;

    std::array<std::byte, ps2hdd::apa::kSectorSize> sector{};
    std::memcpy(sector.data(), &sb, sizeof(sb));
    dev.put_bytes((static_cast<std::uint64_t>(pfs_lba) + ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize, sector);
    dev.put_bytes((static_cast<std::uint64_t>(pfs_lba) + ps2hdd::pfs::kSuperBackupSector) * ps2hdd::apa::kSectorSize, sector);

    ps2hdd::apa::Reader reader(dev);
    const auto apa_result = reader.scan();
    check(apa_result.ok(), "apa_result.ok()");
    check(apa_result.partitions.size() == 2, "apa_result.partitions.size() == 2");

    ps2hdd::ApaVolume volume(dev, apa_result.partitions[1]);
    const auto pfs_result = ps2hdd::pfs::probe(volume);
    check(pfs_result.valid, "pfs_result.valid");
    check(pfs_result.backup_matches, "pfs_result.backup_matches");
    check(pfs_result.super.version == 3, "pfs_result.super.version == 3");
    check(pfs_result.super.zone_size == 8192, "pfs_result.super.zone_size == 8192");
}

void pfs_root_directory_is_read()
{
    constexpr std::uint32_t pfs_lba = 0x40000;
    constexpr std::uint32_t zone_size = 8192;
    constexpr std::uint32_t root_zone = 100;
    constexpr std::uint32_t data_zone = 101;
    constexpr std::uint32_t cfg_zone = 102;

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
    sb.zone_size = zone_size;
    sb.root = {root_zone, 0, 1};
    sb.log = {50, 0, 1};
    std::array<std::byte, ps2hdd::apa::kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &sb, sizeof(sb));
    dev.put_bytes((static_cast<std::uint64_t>(pfs_lba) + ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize, super_sector);
    dev.put_bytes((static_cast<std::uint64_t>(pfs_lba) + ps2hdd::pfs::kSuperBackupSector) * ps2hdd::apa::kSectorSize, super_sector);

    ps2hdd::pfs::Inode root{};
    root.magic = ps2hdd::pfs::kSegdMagic;
    root.inode_block = {root_zone, 0, 1};
    root.last_segment = root.inode_block;
    root.data[0] = root.inode_block;
    root.data[1] = {data_zone, 0, 1};
    root.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 0x01FF);
    root.size = 36;
    root.number_data = 2;
    root.number_blocks = 2;
    root.checksum = ps2hdd::pfs::inode_checksum(root);

    ps2hdd::pfs::Inode cfg{};
    cfg.magic = ps2hdd::pfs::kSegdMagic;
    cfg.inode_block = {cfg_zone, 0, 1};
    cfg.last_segment = cfg.inode_block;
    cfg.data[0] = cfg.inode_block;
    cfg.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 0x01FF);
    cfg.number_data = 1;
    cfg.number_blocks = 1;
    cfg.checksum = ps2hdd::pfs::inode_checksum(cfg);

    const auto partition_base = static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize;
    dev.put_object(partition_base + static_cast<std::uint64_t>(root_zone) * zone_size, root);
    dev.put_object(partition_base + static_cast<std::uint64_t>(cfg_zone) * zone_size, cfg);

    std::array<std::byte, ps2hdd::apa::kSectorSize> dentries{};
    put_dentry(dentries, 0, root_zone, 0, ".", ps2hdd::pfs::kModeDirectory, 12);
    put_dentry(dentries, 12, root_zone, 0, "..", ps2hdd::pfs::kModeDirectory, 12);
    put_dentry(dentries, 24, cfg_zone, 0, "CFG", ps2hdd::pfs::kModeDirectory, 12);
    dev.put_bytes(partition_base + static_cast<std::uint64_t>(data_zone) * zone_size, dentries);

    ps2hdd::apa::Reader apa_reader(dev);
    const auto apa_result = apa_reader.scan();
    check(apa_result.ok(), "synthetic PFS APA scan");
    ps2hdd::ApaVolume volume(dev, apa_result.partitions[1]);
    ps2hdd::pfs::Reader pfs(volume);
    check(pfs.valid(), "PFS reader valid");

    const auto root_node = pfs.root();
    check(root_node.has_value(), "root inode readable");
    check(root_node->inode.size == 36, "root inode size");

    const auto entries = pfs.list_directory(*root_node);
    check(pfs.last_error().empty(), "directory enumeration has no error");
    check(entries.size() == 1, "dot entries hidden");
    check(entries[0].name == "CFG", "CFG entry found");
    check(entries[0].is_directory(), "CFG marked as directory");

    const auto cfg_node = pfs.resolve("/CFG");
    check(cfg_node.has_value(), "CFG path resolves");
    check((cfg_node->inode.mode & ps2hdd::pfs::kModeMask) == ps2hdd::pfs::kModeDirectory,
          "CFG inode is directory");
}

} // namespace

int main()
{
    try {
        valid_chain_is_read();
        bad_checksum_is_rejected();
        cycle_is_detected();
        pfs_superblock_is_probed();
        pfs_root_directory_is_read();
        std::cout << "All APA/PFS tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
