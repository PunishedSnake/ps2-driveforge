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
    std::string display_name() const override { return "pfs-directory-fixture.img"; }

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
    static constexpr std::uint32_t pfs_sectors = 0x10000;
    static constexpr std::uint32_t zone_size = 8192;
    static constexpr std::uint32_t root_inode_zone = 100;
    static constexpr std::uint32_t root_data_zone = 101;
    static constexpr std::uint32_t bitmap_sector = 0x2010;

    Fixture()
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

    LinearDisk disk;
    ps2hdd::apa::ScanResult scan;
};

std::vector<std::byte> pattern(std::size_t size)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 37U + 11U) & 0xFFU);
    }
    return bytes;
}

void recursive_mkdir_then_file_create_roundtrip()
{
    Fixture fixture;
    ps2hdd::pfs::ImageWriter writer(fixture.disk, fixture.scan.partitions[1]);
    check(writer.valid(), "PFS directory writer session must probe fixture");

    const auto created = writer.ensure_directory("/ART/CFG");
    check(created.ok, "recursive PFS mkdir succeeds");
    check(created.created_components == 2, "recursive mkdir creates both missing components");
    check(created.metadata_transactions == 6,
          "each created directory reserves bitmap, initializes data, then publishes inode+dentry");
    check(fixture.bitmap_used(111) && fixture.bitmap_used(112) &&
              fixture.bitmap_used(113) && fixture.bitmap_used(114),
          "recursive mkdir reserves inode and data zones for both directories");

    ps2hdd::ApaVolume volume(fixture.disk, fixture.scan.partitions[1]);
    ps2hdd::pfs::Reader reader(volume);
    const auto art = reader.resolve("/ART");
    const auto cfg = reader.resolve("/ART/CFG");
    check(art.has_value() && cfg.has_value(), "cold reader resolves both created directories");
    check((art->inode.mode & ps2hdd::pfs::kModeMask) == ps2hdd::pfs::kModeDirectory &&
              (cfg->inode.mode & ps2hdd::pfs::kModeMask) == ps2hdd::pfs::kModeDirectory,
          "created nodes are directories");

    const auto cfg_entries = reader.list_directory(*cfg, true);
    const auto self = std::find_if(cfg_entries.begin(), cfg_entries.end(), [](const auto& entry) {
        return entry.name == ".";
    });
    const auto parent = std::find_if(cfg_entries.begin(), cfg_entries.end(), [](const auto& entry) {
        return entry.name == "..";
    });
    check(self != cfg_entries.end() && parent != cfg_entries.end(),
          "created directory exposes dot and dot-dot entries");
    check(self->inode.number == cfg->location.number && self->inode.subpart == cfg->location.subpart,
          "dot entry points at the created directory");
    check(parent->inode.number == art->location.number && parent->inode.subpart == art->location.subpart,
          "dot-dot entry points at parent directory");

    const auto repeated = writer.ensure_directory("ART/CFG");
    check(repeated.ok && repeated.created_components == 0,
          "ensuring an existing directory tree is a no-op");
    check(repeated.metadata_transactions == 0 && repeated.bitmap_chunks_touched == 0,
          "mkdir no-op performs no metadata mutation");

    const auto data = pattern(1400);
    const auto file = writer.write_file("ART/CFG/SLUS_123.45.cfg", data);
    check(file.ok, "normal file writer can use newly-created nested directory");
    check(fixture.bitmap_used(115) && fixture.bitmap_used(116),
          "file create continues from cached free-zone state after mkdir");

    ps2hdd::pfs::Reader verify(volume);
    const auto node = verify.resolve("/ART/CFG/SLUS_123.45.cfg");
    check(node.has_value() && node->inode.size == data.size(), "cold reader resolves created file");
    std::vector<std::byte> actual(data.size());
    check(verify.read(*node, 0, actual) && actual == data,
          "cold reader verifies nested file byte-for-byte");
}

void existing_regular_component_is_refused()
{
    Fixture fixture;
    ps2hdd::pfs::ImageWriter writer(fixture.disk, fixture.scan.partitions[1]);
    check(writer.valid(), "component refusal fixture is valid");

    const auto payload = pattern(32);
    const auto file = writer.write_file("ART", payload);
    check(file.ok, "fixture regular file create succeeds");

    const auto mkdir = writer.ensure_directory("ART/CFG");
    check(!mkdir.ok, "mkdir refuses a regular-file path component");
    check(mkdir.created_components == 0, "refused mkdir publishes no directories");
}

} // namespace

int main()
{
    try {
        recursive_mkdir_then_file_create_roundtrip();
        existing_regular_component_is_refused();
        std::cout << "PFS directory writer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PFS directory writer test failure: " << error.what() << '\n';
        return 1;
    }
}
