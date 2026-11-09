#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/opl_pfs_import.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/tar_archive.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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
    std::string display_name() const override { return "opl-tar-pfs-fixture.img"; }
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
    void put(std::uint64_t offset, std::span<const std::byte> value)
    {
        check(write(offset, value), "fixture byte write");
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
    static constexpr std::uint32_t sectors_per_zone = zone_size / ps2hdd::apa::kSectorSize;
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
        auto mark = [&](std::uint32_t zone) {
            const auto byte = zone / 8U;
            const auto bit = zone % 8U;
            const auto value = std::to_integer<unsigned char>(bitmap[byte]);
            bitmap[byte] = static_cast<std::byte>(value | (1U << bit));
        };
        for (std::uint32_t zone = 0; zone <= 110; ++zone) {
            mark(zone);
        }
        mark(ps2hdd::pfs::kSuperSector / sectors_per_zone);
        mark(bitmap_sector / sectors_per_zone);
        disk.put(base + static_cast<std::uint64_t>(bitmap_sector) * ps2hdd::apa::kSectorSize,
                 bitmap);

        ps2hdd::apa::Reader reader(disk);
        scan = reader.scan();
        check(scan.ok() && scan.partitions.size() == 2, "fixture APA scan");
    }

    LinearDisk disk;
    ps2hdd::apa::ScanResult scan;
};

std::filesystem::path make_temp_root()
{
    const auto root = std::filesystem::temp_directory_path() / "ps2-driveforge-opl-tar-pfs-tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    return root;
}

void write_host(const std::filesystem::path& path, std::string_view data)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "open staged host file");
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    check(static_cast<bool>(output), "write staged host file");
}

ps2hdd::opl::FetchedAsset asset(ps2hdd::opl::AssetKind kind,
                                std::string target,
                                std::string member,
                                const std::filesystem::path& staged)
{
    ps2hdd::opl::FetchedAsset value;
    value.kind = kind;
    value.placement = ps2hdd::opl::Placement::opl_data;
    value.provider = "integration-test";
    value.source_url = "https://example.invalid/asset";
    value.target_path = std::move(target);
    value.archive_member = std::move(member);
    value.staged_path = staged;
    return value;
}

std::vector<std::byte> read_pfs_file(ps2hdd::pfs::Reader& reader, std::string_view path)
{
    const auto node = reader.resolve(path);
    check(node.has_value(), "cold reader resolves imported TAR");
    std::vector<std::byte> bytes(static_cast<std::size_t>(node->inode.size));
    check(bytes.empty() || reader.read(*node, 0, bytes), "cold reader reads complete imported TAR");
    return bytes;
}

std::optional<std::vector<std::byte>> tar_payload(std::span<const std::byte> archive,
                                                  std::string_view wanted)
{
    constexpr std::size_t record = 512;
    std::size_t cursor = 0;
    while (cursor + record <= archive.size()) {
        const auto header = archive.subspan(cursor, record);
        if (std::all_of(header.begin(), header.end(), [](std::byte value) {
                return value == std::byte{0};
            })) {
            break;
        }
        std::size_t name_size = 0;
        while (name_size < 100 && header[name_size] != std::byte{0}) {
            ++name_size;
        }
        const std::string name(reinterpret_cast<const char*>(header.data()), name_size);
        std::uint64_t size = 0;
        for (std::size_t index = 124; index < 136; ++index) {
            const auto ch = std::to_integer<unsigned char>(header[index]);
            if (ch == 0 || ch == static_cast<unsigned char>(' ')) {
                continue;
            }
            check(ch >= static_cast<unsigned char>('0') && ch <= static_cast<unsigned char>('7'),
                  "TAR size is octal in integration fixture");
            size = size * 8U + (ch - static_cast<unsigned char>('0'));
        }
        const auto payload_size = static_cast<std::size_t>(size);
        const auto padded = ((payload_size + record - 1U) / record) * record;
        check(cursor + record <= archive.size() && padded <= archive.size() - (cursor + record),
              "TAR member stays inside archive bounds");
        if (name == wanted) {
            return std::vector<std::byte>(
                archive.begin() + static_cast<std::ptrdiff_t>(cursor + record),
                archive.begin() + static_cast<std::ptrdiff_t>(cursor + record + payload_size));
        }
        cursor += record + padded;
    }
    return std::nullopt;
}

std::vector<std::byte> as_bytes(std::string_view value)
{
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(value.data()),
                                  reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

void create_then_update_opl_tar_containers()
{
    Fixture fixture;
    const auto temp = make_temp_root();
    const auto cover1 = temp / "cover1.bin";
    const auto cfg = temp / "game.cfg";
    write_host(cover1, "COVER-V1");
    write_host(cfg, "DMA=7\nMode=1\n");

    std::vector<ps2hdd::opl::FetchedAsset> first;
    first.push_back(asset(ps2hdd::opl::AssetKind::artwork_cover,
                          "ART/art.tar", "SLUS_123.45_COV.png", cover1));
    first.push_back(asset(ps2hdd::opl::AssetKind::cfg,
                          "CFG/cfg.tar", "SLUS_123.45.cfg", cfg));
    const auto initial = ps2hdd::opl::import_fetched_assets(
        fixture.disk, fixture.scan.partitions[1], first);
    check(initial.ok && !initial.partial && initial.archives.size() == 2,
          "first TAR-layout OPL import commits both containers");

    {
        ps2hdd::ApaVolume volume(fixture.disk, fixture.scan.partitions[1]);
        ps2hdd::pfs::Reader reader(volume);
        const auto art = read_pfs_file(reader, "/ART/art.tar");
        const auto cfg_tar = read_pfs_file(reader, "/CFG/cfg.tar");
        check(ps2hdd::tar::update_archive(art, {}).ok &&
                  ps2hdd::tar::update_archive(cfg_tar, {}).ok,
              "cold reader sees structurally valid TAR containers");
        const auto cover_payload = tar_payload(art, "SLUS_123.45_COV.png");
        const auto cfg_payload = tar_payload(cfg_tar, "SLUS_123.45.cfg");
        check(cover_payload && *cover_payload == as_bytes("COVER-V1"),
              "ART TAR contains exact first cover bytes");
        check(cfg_payload && *cfg_payload == as_bytes("DMA=7\nMode=1\n"),
              "CFG TAR contains exact config bytes");
    }

    const auto cover2 = temp / "cover2.bin";
    const auto icon = temp / "icon.bin";
    write_host(cover2, "COVER-V2-LONGER");
    write_host(icon, "ICON-V1");
    std::vector<ps2hdd::opl::FetchedAsset> second;
    second.push_back(asset(ps2hdd::opl::AssetKind::artwork_cover,
                           "ART/art.tar", "SLUS_123.45_COV.png", cover2));
    second.push_back(asset(ps2hdd::opl::AssetKind::artwork_icon,
                           "ART/art.tar", "SLUS_123.45_ICO.png", icon));
    const auto updated = ps2hdd::opl::import_fetched_assets(
        fixture.disk, fixture.scan.partitions[1], second);
    check(updated.ok && updated.archives.size() == 1,
          "second import copy-on-write updates existing ART TAR");
    check(updated.archives[0].replaced_members == 1 &&
              updated.archives[0].added_members == 1,
          "TAR update reports one replacement and one addition");

    {
        ps2hdd::ApaVolume volume(fixture.disk, fixture.scan.partitions[1]);
        ps2hdd::pfs::Reader reader(volume);
        const auto art = read_pfs_file(reader, "/ART/art.tar");
        const auto cfg_tar = read_pfs_file(reader, "/CFG/cfg.tar");
        check(ps2hdd::tar::update_archive(art, {}).ok,
              "updated ART TAR reparses after cold reopen");
        const auto cover_payload = tar_payload(art, "SLUS_123.45_COV.png");
        const auto icon_payload = tar_payload(art, "SLUS_123.45_ICO.png");
        const auto cfg_payload = tar_payload(cfg_tar, "SLUS_123.45.cfg");
        check(cover_payload && *cover_payload == as_bytes("COVER-V2-LONGER"),
              "replacement cover survives PFS COW and cold read");
        check(icon_payload && *icon_payload == as_bytes("ICON-V1"),
              "new icon survives PFS COW and cold read");
        check(cfg_payload && *cfg_payload == as_bytes("DMA=7\nMode=1\n"),
              "unrelated CFG TAR remains unchanged by ART update");
    }

    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
}

} // namespace

int main()
{
    try {
        create_then_update_opl_tar_containers();
        std::cout << "OPL TAR/PFS integration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OPL TAR/PFS integration test failure: " << error.what() << '\n';
        return 1;
    }
}
