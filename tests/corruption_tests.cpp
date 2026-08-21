#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kPfsLba = 0x4000;
constexpr std::uint32_t kPfsLength = 0x8000;
constexpr std::uint32_t kZoneSize = 8192;
constexpr std::uint32_t kRootZone = 700;
constexpr std::uint32_t kRootDataZone = 701;
constexpr std::uint64_t kDeviceBytes = 32ULL * 1024ULL * 1024ULL;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class DenseDevice final : public ps2hdd::BlockDevice {
public:
    DenseDevice() : bytes_(static_cast<std::size_t>(kDeviceBytes)) {}

    [[nodiscard]] std::uint64_t size_bytes() const override { return bytes_.size(); }
    [[nodiscard]] std::string display_name() const override { return "corruption-fixture"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::memcpy(out.data(), bytes_.data() + static_cast<std::ptrdiff_t>(offset), out.size());
        return true;
    }

    template <typename T>
    void put(std::uint64_t offset, const T& value)
    {
        const auto raw = std::as_bytes(std::span{&value, 1});
        check(offset <= bytes_.size() && raw.size() <= bytes_.size() - static_cast<std::size_t>(offset),
              "fixture write in bounds");
        std::memcpy(bytes_.data() + static_cast<std::ptrdiff_t>(offset), raw.data(), raw.size());
    }

    void put_bytes(std::uint64_t offset, std::span<const std::byte> raw)
    {
        check(offset <= bytes_.size() && raw.size() <= bytes_.size() - static_cast<std::size_t>(offset),
              "fixture bytes in bounds");
        std::memcpy(bytes_.data() + static_cast<std::ptrdiff_t>(offset), raw.data(), raw.size());
    }

private:
    std::vector<std::byte> bytes_;
};

ps2hdd::apa::Header header(std::string_view id, std::uint16_t type, std::uint32_t start,
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

void put_free_dentry(std::array<std::byte, ps2hdd::apa::kSectorSize>& sector,
                     std::size_t offset, std::uint16_t allocated)
{
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 6), &allocated, sizeof(allocated));
}

DenseDevice valid_fixture()
{
    DenseDevice dev;
    auto mbr = header("__mbr", ps2hdd::apa::kTypeMbr, 0, kPfsLba, 0, kPfsLba);
    constexpr char sony[] = "Sony Computer Entertainment Inc.";
    std::memcpy(mbr.mbr.magic, sony, std::min(sizeof(sony), sizeof(mbr.mbr.magic)));
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    auto pfs_header = header("+TEST", ps2hdd::apa::kTypePfs, kPfsLba, kPfsLength, 0, 0);
    dev.put(0, mbr);
    dev.put(static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize, pfs_header);

    ps2hdd::pfs::SuperBlock sb{};
    sb.magic = ps2hdd::pfs::kSuperMagic;
    sb.version = 3;
    sb.zone_size = kZoneSize;
    sb.root = {kRootZone, 0, 1};
    sb.log = {650, 0, 1};
    std::array<std::byte, ps2hdd::apa::kSectorSize> super{};
    std::memcpy(super.data(), &sb, sizeof(sb));
    const auto base = static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize;
    dev.put_bytes(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize, super);
    dev.put_bytes(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperBackupSector) * ps2hdd::apa::kSectorSize, super);

    ps2hdd::pfs::Inode root{};
    root.magic = ps2hdd::pfs::kSegdMagic;
    root.inode_block = {kRootZone, 0, 1};
    root.last_segment = root.inode_block;
    root.data[0] = root.inode_block;
    root.data[1] = {kRootDataZone, 0, 1};
    root.mode = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 0x01FF);
    root.size = ps2hdd::apa::kSectorSize;
    root.number_data = 2;
    root.number_blocks = 2;
    root.checksum = ps2hdd::pfs::inode_checksum(root);
    dev.put(base + static_cast<std::uint64_t>(kRootZone) * kZoneSize, root);

    std::array<std::byte, ps2hdd::apa::kSectorSize> directory{};
    std::uint32_t inode = kRootZone;
    std::uint16_t raw = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 12);
    std::memcpy(directory.data(), &inode, sizeof(inode));
    directory[4] = std::byte{0};
    directory[5] = std::byte{1};
    std::memcpy(directory.data() + 6, &raw, sizeof(raw));
    directory[8] = std::byte{'.'};
    std::memcpy(directory.data() + 12, &inode, sizeof(inode));
    directory[16] = std::byte{0};
    directory[17] = std::byte{2};
    std::memcpy(directory.data() + 18, &raw, sizeof(raw));
    directory[20] = std::byte{'.'};
    directory[21] = std::byte{'.'};
    put_free_dentry(directory, 24, static_cast<std::uint16_t>(ps2hdd::apa::kSectorSize - 24));
    dev.put_bytes(base + static_cast<std::uint64_t>(kRootDataZone) * kZoneSize, directory);
    return dev;
}

ps2hdd::apa::Partition pfs_partition(DenseDevice& dev)
{
    ps2hdd::apa::Reader reader(dev);
    const auto scan = reader.scan();
    check(scan.ok() && scan.partitions.size() == 2, "valid corruption fixture scans");
    return scan.partitions[1];
}

void apa_extent_outside_device()
{
    auto dev = valid_fixture();
    auto broken = header("+TEST", ps2hdd::apa::kTypePfs, kPfsLba, 0xFFFFFFF0U, 0, 0);
    dev.put(static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize, broken);
    ps2hdd::apa::Reader reader(dev);
    const auto scan = reader.scan();
    check(!scan.ok(), "APA partition extending outside device is fatal");
}

void apa_sub_extent_outside_device()
{
    auto dev = valid_fixture();
    auto main = header("+TEST", ps2hdd::apa::kTypePfs, kPfsLba, kPfsLength, 0, 0);
    main.nsub = 1;
    main.subs[0] = {0xFFFF0000U, 0x1000U};
    main.checksum = ps2hdd::apa::checksum(main);
    dev.put(static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize, main);
    ps2hdd::apa::Reader reader(dev);
    const auto scan = reader.scan();
    check(!scan.ok(), "APA subpartition extending outside device is fatal");
}

void invalid_pfs_zone_size()
{
    auto dev = valid_fixture();
    const auto partition = pfs_partition(dev);
    const auto base = static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize;
    ps2hdd::pfs::SuperBlock sb{};
    sb.magic = ps2hdd::pfs::kSuperMagic;
    sb.version = 3;
    sb.zone_size = 6000;
    sb.root = {kRootZone, 0, 1};
    std::array<std::byte, ps2hdd::apa::kSectorSize> sector{};
    std::memcpy(sector.data(), &sb, sizeof(sb));
    dev.put_bytes(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize, sector);
    ps2hdd::ApaVolume volume(dev, partition);
    const auto probe = ps2hdd::pfs::probe(volume);
    check(!probe.valid, "non-power-of-two PFS zone rejected");
}

void root_missing_subpartition()
{
    auto dev = valid_fixture();
    const auto partition = pfs_partition(dev);
    const auto base = static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize;
    ps2hdd::pfs::SuperBlock sb{};
    sb.magic = ps2hdd::pfs::kSuperMagic;
    sb.version = 3;
    sb.zone_size = kZoneSize;
    sb.num_subs = 0;
    sb.root = {kRootZone, 1, 1};
    std::array<std::byte, ps2hdd::apa::kSectorSize> sector{};
    std::memcpy(sector.data(), &sb, sizeof(sb));
    dev.put_bytes(base + static_cast<std::uint64_t>(ps2hdd::pfs::kSuperSector) * ps2hdd::apa::kSectorSize, sector);
    ps2hdd::ApaVolume volume(dev, partition);
    check(!ps2hdd::pfs::probe(volume).valid, "root referencing absent PFS subpartition rejected");
}

void bad_inode_checksum()
{
    auto dev = valid_fixture();
    const auto partition = pfs_partition(dev);
    const auto base = static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize;
    ps2hdd::pfs::Inode broken{};
    broken.magic = ps2hdd::pfs::kSegdMagic;
    broken.inode_block = {kRootZone, 0, 1};
    broken.data[0] = broken.inode_block;
    broken.number_data = 1;
    broken.mode = ps2hdd::pfs::kModeDirectory;
    broken.checksum = 0x12345678U;
    dev.put(base + static_cast<std::uint64_t>(kRootZone) * kZoneSize, broken);
    ps2hdd::ApaVolume volume(dev, partition);
    ps2hdd::pfs::Reader reader(volume);
    check(reader.valid(), "PFS superblock remains valid for inode corruption test");
    check(!reader.root().has_value(), "bad inode checksum rejected");
}

void malformed_directory_entry()
{
    auto dev = valid_fixture();
    const auto partition = pfs_partition(dev);
    const auto base = static_cast<std::uint64_t>(kPfsLba) * ps2hdd::apa::kSectorSize;
    std::array<std::byte, ps2hdd::apa::kSectorSize> directory{};
    std::uint32_t inode = kRootZone;
    std::uint16_t malformed = static_cast<std::uint16_t>(ps2hdd::pfs::kModeDirectory | 10);
    std::memcpy(directory.data(), &inode, sizeof(inode));
    directory[5] = std::byte{1};
    std::memcpy(directory.data() + 6, &malformed, sizeof(malformed));
    directory[8] = std::byte{'.'};
    dev.put_bytes(base + static_cast<std::uint64_t>(kRootDataZone) * kZoneSize, directory);

    ps2hdd::ApaVolume volume(dev, partition);
    ps2hdd::pfs::Reader reader(volume);
    auto root = reader.root();
    check(root.has_value(), "root inode still readable");
    const auto entries = reader.list_directory(*root);
    check(entries.empty() && !reader.last_error().empty(), "malformed dentry allocation rejected");
}

} // namespace

int main()
{
    try {
        apa_extent_outside_device();
        apa_sub_extent_outside_device();
        invalid_pfs_zone_size();
        root_missing_subpartition();
        bad_inode_checksum();
        malformed_directory_entry();
        std::cout << "Corruption regression corpus passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
