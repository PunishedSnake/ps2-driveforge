#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_batch.hpp"

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
    std::string display_name() const override { return "pfs-batch-fixture.img"; }

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
    static constexpr std::uint32_t art_inode_zone = 102;
    static constexpr std::uint32_t art_data_zone = 103;
    static constexpr std::uint32_t bitmap_sector = 0x2010;

    Fixture()
        : disk(static_cast<std::size_t>(pfs_lba + pfs_sectors + 0x1000) *
               ps2hdd::apa::kSectorSize)
    {
        auto mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x4000, pfs_lba, pfs_lba);
        std::memcpy(mbr.mbr.magic, "Sony Computer Entertainment Inc.", 32);
        mbr.checksum = ps2hdd::apa::checksum(mbr);
        const auto opl = make_header("+OPL", ps2hdd::apa::kTypePfs, pfs_lba,
                                     pfs_sectors, 0, 0);
        disk.put_object(0, mbr);
        disk.put_object(static_cast<std::uint64_t>(pfs_lba) * ps2hdd::apa::kSectorSize, opl);

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
        root.size = 512;
        root.number_blocks = 2;
        root.number_data = 2;
        root.number_segdesg = 1;
        root.checksum = ps2hdd::pfs::inode_checksum(root);
        disk.put_object(base + static_cast<std::uint64_t>(root_inode_zone) * zone_size, root);

        ps2hdd::pfs::Inode art{};
        art.magic = ps2hdd::pfs::kSegdMagic;
        art.inode_block = {art_inode_zone, 0, 1};
        art.last_segment = art.inode_block;
        art.data[0] = art.inode_block;
        art.data[1] = {art_data_zone, 0, 1};
        art.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 0x01FFU);
        art.size = 512;
        art.number_blocks = 2;
        art.number_data = 2;
        art.number_segdesg = 1;
        art.checksum = ps2hdd::pfs::inode_checksum(art);
        disk.put_object(base + static_cast<std::uint64_t>(art_inode_zone) * zone_size, art);

        std::array<std::byte, ps2hdd::apa::kSectorSize> root_dir{};
        put_dentry(root_dir, 0, root_inode_zone, ".", ps2hdd::pfs::kModeDirectory, 12);
        put_dentry(root_dir, 12, root_inode_zone, "..", ps2hdd::pfs::kModeDirectory, 12);
        put_dentry(root_dir, 24, art_inode_zone, "ART", ps2hdd::pfs::kModeDirectory, 488);
        disk.put(base + static_cast<std::uint64_t>(root_data_zone) * zone_size, root_dir);

        std::array<std::byte, ps2hdd::apa::kSectorSize> art_dir{};
        put_dentry(art_dir, 0, art_inode_zone, ".", ps2hdd::pfs::kModeDirectory, 12);
        put_dentry(art_dir, 12, root_inode_zone, "..", ps2hdd::pfs::kModeDirectory, 500);
        disk.put(base + static_cast<std::uint64_t>(art_data_zone) * zone_size, art_dir);

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

    LinearDisk disk;
    ps2hdd::apa::ScanResult scan;
};

std::vector<std::byte> pattern(std::size_t size, unsigned seed)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 29U + seed) & 0xFFU);
    }
    return bytes;
}

void optional_failure_does_not_destroy_batch_progress()
{
    Fixture fixture;
    ps2hdd::pfs::ImageWriter writer(fixture.disk, fixture.scan.partitions[1]);
    check(writer.valid(), "batch writer fixture is valid");

    std::vector<ps2hdd::pfs::BatchFile> files;
    files.push_back({"ART/SLUS_123.45_COV.png", pattern(1500, 3), {}, true});
    files.push_back({"MISSING/optional.bin", pattern(64, 9), {}, false});
    files.push_back({"ART/SLUS_123.45_ICO.png", pattern(900, 17), {}, true});

    const auto result = ps2hdd::pfs::write_batch(writer, files);
    check(result.ok, "optional item failure must not fail required batch contract");
    check(result.partial, "optional failure is reported as partial");
    check(result.attempted == 3 && result.succeeded == 2 && result.failed == 1,
          "batch counts all attempted items");
    check(result.optional_failed == 1, "optional failure counter");
    check(result.bytes_committed == 2400, "batch committed-byte accounting");
    check(result.metadata_transactions >= 6, "batch aggregates metadata transaction counts");

    ps2hdd::ApaVolume volume(fixture.disk, fixture.scan.partitions[1]);
    ps2hdd::pfs::Reader reader(volume);
    check(reader.resolve("ART/SLUS_123.45_COV.png").has_value(), "first required file is visible");
    check(reader.resolve("ART/SLUS_123.45_ICO.png").has_value(), "later required file is visible");
}

void required_failure_stops_before_later_items()
{
    Fixture fixture;
    ps2hdd::pfs::ImageWriter writer(fixture.disk, fixture.scan.partitions[1]);
    check(writer.valid(), "required-failure fixture is valid");

    std::vector<ps2hdd::pfs::BatchFile> files;
    files.push_back({"MISSING/required.bin", pattern(32, 1), {}, true});
    files.push_back({"ART/SHOULD_NOT_EXIST.bin", pattern(64, 2), {}, true});

    const auto result = ps2hdd::pfs::write_batch(writer, files);
    check(!result.ok && result.partial, "required failure fails and truncates batch");
    check(result.attempted == 1 && result.failed == 1 && result.succeeded == 0,
          "later batch item is not attempted");

    ps2hdd::ApaVolume volume(fixture.disk, fixture.scan.partitions[1]);
    ps2hdd::pfs::Reader reader(volume);
    check(!reader.resolve("ART/SHOULD_NOT_EXIST.bin").has_value(),
          "later required file was not published");
}

} // namespace

int main()
{
    try {
        optional_failure_does_not_destroy_batch_progress();
        required_failure_stops_before_later_items();
        std::cout << "PFS batch writer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PFS batch writer test failure: " << error.what() << '\n';
        return 1;
    }
}
