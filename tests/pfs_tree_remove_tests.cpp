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
    std::string display_name() const override { return "pfs-tree-remove-fixture.img"; }
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
    template <typename T> void put_object(std::uint64_t offset, const T& value)
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
    static constexpr std::uint32_t pfs_sectors = 0x10000;
    static constexpr std::uint32_t zone_size = 8192;
    static constexpr std::uint32_t root_inode_zone = 100;
    static constexpr std::uint32_t root_data_zone = 101;
    static constexpr std::uint32_t bitmap_sector = 0x2010;

    Fixture()
        : disk(static_cast<std::size_t>(pfs_lba + pfs_sectors + 0x1000) * ps2hdd::apa::kSectorSize)
    {
        auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x4000, pfs_lba, pfs_lba);
        std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
        mbr.checksum = ps2hdd::apa::checksum(mbr);
        const auto pfs = make_header("+OPL", ps2hdd::apa::kTypePfs, pfs_lba, pfs_sectors, 0, 0);
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
        disk.put(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize,
                 super_sector);
        disk.put(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperBackupSector) * ps2hdd::apa::kSectorSize,
                 super_sector);

        ps2hdd::pfs::Inode root{};
        root.magic = ps2hdd::pfs::kSegdMagic;
        root.inode_block = {root_inode_zone, 0, 1};
        root.last_segment = root.inode_block;
        root.data[0] = root.inode_block;
        root.data[1] = {root_data_zone, 0, 1};
        root.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 0x01FFU);
        root.size = ps2hdd::apa::kSectorSize;
        root.number_blocks = 2;
        root.number_data = 2;
        root.number_segdesg = 1;
        root.checksum = ps2hdd::pfs::inode_checksum(root);
        disk.put_object(base + static_cast<std::uint64_t>(root_inode_zone) * zone_size, root);

        std::array<std::byte, ps2hdd::apa::kSectorSize> root_dir{};
        put_dentry(root_dir, 0, root_inode_zone, ".", ps2hdd::pfs::kModeDirectory, 12);
        put_dentry(root_dir, 12, root_inode_zone, "..", ps2hdd::pfs::kModeDirectory, 500);
        disk.put(base + static_cast<std::uint64_t>(root_data_zone) * zone_size, root_dir);

        std::array<std::byte, ps2hdd::pfs::kMetadataSize> bitmap{};
        for (std::uint32_t zone = 0; zone <= 110; ++zone) {
            const auto byte = zone / 8U;
            const auto bit = zone % 8U;
            const auto value = std::to_integer<unsigned char>(bitmap[byte]);
            bitmap[byte] = static_cast<std::byte>(value | (1U << bit));
        }
        disk.put(base + static_cast<std::uint64_t>(bitmap_sector) * ps2hdd::apa::kSectorSize, bitmap);

        ps2hdd::apa::Reader reader(disk);
        scan = reader.scan();
        check(scan.ok() && scan.partitions.size() == 2, "fixture APA scan");
    }

    LinearDisk disk;
    ps2hdd::apa::ScanResult scan;
};

std::vector<std::byte> payload(std::size_t size, std::uint8_t salt)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<std::byte>((index * 13U + salt) & 0xFFU);
    }
    return bytes;
}

void empty_directory_refuses_nonempty_then_recursive_remove_succeeds()
{
    Fixture fixture;
    ps2hdd::pfs::ImageWriter writer(fixture.disk, fixture.scan.partitions[1]);
    check(writer.ensure_directory_full("/ART/CFG").ok, "nested tree create succeeds");
    check(writer.write_file_complete("/ART/a.bin", payload(900, 3)).ok, "first tree file create");
    check(writer.write_file_complete("/ART/CFG/b.bin", payload(1400, 5)).ok, "second tree file create");

    const auto refused = writer.remove_empty_directory("/ART");
    check(!refused.ok, "non-empty directory cannot be removed as empty");

    const auto removed = writer.remove_tree("/ART");
    check(removed.ok && !removed.partial, "recursive tree removal completes");
    check(removed.files_removed == 2 && removed.directories_removed == 2,
          "recursive removal reports exact file and directory counts");

    ps2hdd::ApaVolume volume(fixture.disk, fixture.scan.partitions[1]);
    ps2hdd::pfs::Reader reader(volume);
    check(!reader.resolve("/ART"), "cold reader no longer resolves removed tree");
    const auto root = reader.root();
    check(root.has_value(), "root remains valid after subtree removal");
    const auto entries = reader.list_directory(*root, false);
    check(reader.last_error().empty() && entries.empty(), "root namespace is empty after subtree removal");
}

void root_removal_is_refused()
{
    Fixture fixture;
    ps2hdd::pfs::ImageWriter writer(fixture.disk, fixture.scan.partitions[1]);
    check(!writer.remove_empty_directory("/").ok, "empty-directory API refuses PFS root");
    check(!writer.remove_tree("/").ok, "recursive API refuses PFS root");
}

} // namespace

int main()
{
    try {
        empty_directory_refuses_nonempty_then_recursive_remove_succeeds();
        root_removal_is_refused();
        std::cout << "PFS tree removal tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PFS tree removal test failure: " << error.what() << '\n';
        return 1;
    }
}
