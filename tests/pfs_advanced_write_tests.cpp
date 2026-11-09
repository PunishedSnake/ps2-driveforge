#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_write.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
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

class LinearDisk final : public ps2hdd::WritableBlockDevice {
public:
    explicit LinearDisk(std::size_t bytes) : bytes_(bytes) {}

    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "pfs-advanced-fixture.img"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(out.data(), bytes_.data() + static_cast<std::size_t>(offset), out.size());
        return true;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        if (offset > bytes_.size() || in.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(bytes_.data() + static_cast<std::size_t>(offset), in.data(), in.size());
        return true;
    }

    bool flush() override { return true; }

    template <typename T>
    void put_object(std::uint64_t offset, const T& value)
    {
        check(write(offset, std::as_bytes(std::span{&value, 1})), "fixture object write");
    }

    void put(std::uint64_t offset, std::span<const std::byte> bytes)
    {
        check(write(offset, bytes), "fixture byte write");
    }

private:
    std::vector<std::byte> bytes_;
};

ps2hdd::apa::Header make_header(const char* id, std::uint16_t type,
                                std::uint32_t start, std::uint32_t length,
                                std::uint32_t prev, std::uint32_t next)
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

void put_dentry(std::span<std::byte> sector, std::size_t offset, std::uint32_t inode,
                std::string_view name, std::uint16_t mode, std::uint16_t allocated)
{
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset), &inode, sizeof(inode));
    sector[offset + 4] = std::byte{0};
    sector[offset + 5] = static_cast<std::byte>(name.size());
    const auto raw = static_cast<std::uint16_t>(mode | allocated);
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 6), &raw, sizeof(raw));
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 8), name.data(), name.size());
}

struct Fixture {
    static constexpr std::uint32_t pfs_lba = 0x4000;
    static constexpr std::uint32_t pfs_sectors = 0x20000;
    static constexpr std::uint32_t zone_size = 8192;
    static constexpr std::uint32_t root_inode_zone = 100;
    static constexpr std::uint32_t root_data_zone = 101;
    static constexpr std::uint32_t bitmap_sector = 0x2010;
    static constexpr std::uint32_t zones = pfs_sectors / (zone_size / ps2hdd::apa::kSectorSize);

    Fixture(bool packed_root = false, bool fragment_free_space = false)
        : disk(static_cast<std::size_t>(pfs_lba + pfs_sectors + 0x1000) *
               ps2hdd::apa::kSectorSize)
    {
        auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x4000, pfs_lba, pfs_lba);
        std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
        mbr.checksum = ps2hdd::apa::checksum(mbr);
        const auto pfs = make_header("+OPL", ps2hdd::apa::kTypePfs, pfs_lba,
                                     pfs_sectors, 0, 0);
        disk.put_object(0, mbr);
        disk.put_object(static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize, pfs);

        const auto base = static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize;
        ps2hdd::pfs::SuperBlock super{};
        super.magic = ps2hdd::pfs::kSuperMagic;
        super.version = ps2hdd::pfs::kFormatVersion;
        super.zone_size = zone_size;
        super.root = {root_inode_zone, 0, 1};
        super.log = {50, 0, 1};
        std::array<std::byte, ps2hdd::apa::kSectorSize> super_sector{};
        std::memcpy(super_sector.data(), &super, sizeof(super));
        disk.put(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) *
                            ps2hdd::apa::kSectorSize,
                 super_sector);
        disk.put(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperBackupSector) *
                            ps2hdd::apa::kSectorSize,
                 super_sector);

        ps2hdd::pfs::Inode root{};
        root.magic = ps2hdd::pfs::kSegdMagic;
        root.inode_block = {root_inode_zone, 0, 1};
        root.last_segment = root.inode_block;
        root.data[0] = root.inode_block;
        root.data[1] = {root_data_zone, 0, 1};
        root.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 0x01FFU);
        root.attr = 0x00A0U;
        root.size = ps2hdd::apa::kSectorSize;
        root.number_blocks = 2;
        root.number_data = 2;
        root.number_segdesg = 1;
        root.checksum = ps2hdd::pfs::inode_checksum(root);
        disk.put_object(base + static_cast<std::uint64_t>(root_inode_zone) * zone_size, root);

        std::array<std::byte, ps2hdd::apa::kSectorSize> root_dir{};
        if (packed_root) {
            put_dentry(root_dir, 0, root_inode_zone, ".", ps2hdd::pfs::kModeDirectory, 12);
            put_dentry(root_dir, 12, root_inode_zone, "..", ps2hdd::pfs::kModeDirectory, 12);
            const std::string a(255, 'A');
            const std::string b(216, 'B');
            put_dentry(root_dir, 24, root_inode_zone, a, ps2hdd::pfs::kModeRegular, 264);
            put_dentry(root_dir, 288, root_inode_zone, b, ps2hdd::pfs::kModeRegular, 224);
        } else {
            put_dentry(root_dir, 0, root_inode_zone, ".", ps2hdd::pfs::kModeDirectory, 12);
            put_dentry(root_dir, 12, root_inode_zone, "..", ps2hdd::pfs::kModeDirectory, 500);
        }
        disk.put(base + static_cast<std::uint64_t>(root_data_zone) * zone_size, root_dir);

        std::array<std::byte, ps2hdd::pfs::kMetadataSize> bitmap{};
        auto mark = [&](std::uint32_t zone) {
            const auto byte = zone / 8U;
            const auto bit = zone % 8U;
            const auto value = std::to_integer<unsigned char>(bitmap[byte]);
            bitmap[byte] = static_cast<std::byte>(value | (1U << bit));
        };
        for (std::uint32_t zone = 0; zone <= 110; ++zone) {
            mark(zone);
        }
        if (fragment_free_space) {
            for (std::uint32_t zone = 112; zone < zones; zone += 2U) {
                mark(zone);
            }
        }
        disk.put(base + static_cast<std::uint64_t>(bitmap_sector) * ps2hdd::apa::kSectorSize,
                 bitmap);

        ps2hdd::apa::Reader reader(disk);
        scan = reader.scan();
        check(scan.ok() && scan.partitions.size() == 2, "fixture APA scan");
    }

    bool bitmap_used(std::uint32_t zone)
    {
        std::array<std::byte, ps2hdd::pfs::kMetadataSize> bytes{};
        const auto base = static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize;
        check(disk.read(base + static_cast<std::uint64_t>(bitmap_sector) * ps2hdd::apa::kSectorSize,
                        bytes),
              "read fixture bitmap");
        const auto value = std::to_integer<unsigned char>(bytes[zone / 8U]);
        return (value & (1U << (zone % 8U))) != 0;
    }

    ps2hdd::ApaVolume volume()
    {
        return ps2hdd::ApaVolume(disk, scan.partitions[1]);
    }

    LinearDisk disk;
    ps2hdd::apa::ScanResult scan;
};

std::vector<std::byte> pattern(std::size_t size)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<std::byte>((index * 29U + 17U) & 0xFFU);
    }
    return bytes;
}

void directory_grows_without_allocating_a_new_zone_when_capacity_exists()
{
    Fixture fixture(true, false);
    ps2hdd::pfs::ImageWriter writer(fixture.disk, fixture.scan.partitions[1]);
    check(writer.valid(), "advanced directory fixture probes");

    const auto result = writer.ensure_directory_full("/ART");
    check(result.ok && result.created_components == 1,
          "full mkdir grows packed parent and creates child");

    auto volume = fixture.volume();
    ps2hdd::pfs::Reader reader(volume);
    const auto root = reader.root();
    const auto art = reader.resolve("/ART");
    check(root.has_value() && art.has_value(), "cold reader sees grown root and ART");
    check(root->inode.size == 1024, "packed root grows by exactly one sector");
    check(root->inode.number_data == 2 && root->inode.data[1].count == 1,
          "directory growth consumes spare capacity before allocating another zone");
}

void fragmented_create_uses_multiple_direct_extents()
{
    Fixture fixture(false, true);
    ps2hdd::pfs::ImageWriter writer(fixture.disk, fixture.scan.partitions[1]);
    check(writer.valid(), "fragmented writer fixture probes");

    const auto data = pattern(Fixture::zone_size * 2U);
    const auto result = writer.write_file_full("/FRAG.BIN", data);
    check(result.ok, "full writer succeeds when no three-zone contiguous run exists");

    auto volume = fixture.volume();
    ps2hdd::pfs::Reader reader(volume);
    const auto node = reader.resolve("/FRAG.BIN");
    check(node.has_value(), "cold reader resolves fragmented file");
    check(node->inode.number_data == 3,
          "fragmented file stores two direct data descriptors after inode self descriptor");
    check(node->inode.data[1].number + node->inode.data[1].count != node->inode.data[2].number,
          "fragmented file data descriptors are physically non-contiguous");
    std::vector<std::byte> actual(data.size());
    check(reader.read(*node, 0, actual) && actual == data,
          "cold reader verifies fragmented file byte-for-byte");
}

void fast_file_unlink_releases_bitmap_and_reuses_namespace_slot()
{
    Fixture fixture;
    ps2hdd::pfs::ImageWriter writer(fixture.disk, fixture.scan.partitions[1]);
    const auto data = pattern(1400);
    const auto created = writer.write_file_full("/DELETE.ME", data);
    check(created.ok, "unlink fixture file create succeeds");
    check(fixture.bitmap_used(111) && fixture.bitmap_used(112),
          "created file owns inode and payload zones");

    const auto removed = writer.remove_file("/DELETE.ME");
    check(removed.ok, "fast PFS file unlink succeeds");
    check(!fixture.bitmap_used(111) && !fixture.bitmap_used(112),
          "unlink releases inode and payload bitmap zones after namespace removal");

    auto volume = fixture.volume();
    ps2hdd::pfs::Reader reader(volume);
    check(!reader.resolve("/DELETE.ME"), "cold reader no longer resolves unlinked file");

    const auto replacement = writer.write_file_full("/REUSED.BIN", data);
    check(replacement.ok, "writer reuses freed directory entry and allocation space");
    check(fixture.bitmap_used(111) && fixture.bitmap_used(112),
          "freed low zones are reusable without rebuilding the filesystem catalog");
}

} // namespace

int main()
{
    try {
        directory_grows_without_allocating_a_new_zone_when_capacity_exists();
        fragmented_create_uses_multiple_direct_extents();
        fast_file_unlink_releases_bitmap_and_reuses_namespace_slot();
        std::cout << "Advanced PFS write tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Advanced PFS write test failure: " << error.what() << '\n';
        return 1;
    }
}
