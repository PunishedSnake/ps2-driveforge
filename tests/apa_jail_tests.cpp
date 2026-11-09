// Frieren APA-Jail regressions: hybrid APA + MBR layout detection, allocation
// confinement and sector-0 preservation. Fixture helpers are shared verbatim
// with image_game_deploy_tests.cpp (copied to keep executables standalone).

#include "ps2hdd/image_game_deploy.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_allocation.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/hdl.hpp"
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
#include <map>
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

void store_u32_both(std::byte* p, std::uint32_t value)
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
    p[4] = p[3];
    p[5] = p[2];
    p[6] = p[1];
    p[7] = p[0];
}

class MemoryIso final : public ps2hdd::BlockDevice {
public:
    explicit MemoryIso(std::size_t bytes) : bytes_(bytes) {}
    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "deploy-game.iso"; }
    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }
    std::span<std::byte> bytes() { return bytes_; }

private:
    std::vector<std::byte> bytes_;
};

class SparseOverlayDisk final : public ps2hdd::WritableBlockDevice {
public:
    explicit SparseOverlayDisk(std::uint64_t size) : size_(size) {}
    std::uint64_t size_bytes() const override { return size_; }
    std::string display_name() const override { return "deploy-target.img"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > size_ || out.size() > size_ - offset) {
            return false;
        }
        std::fill(out.begin(), out.end(), std::byte{0});
        const std::uint64_t request_end = offset + out.size();
        for (const auto& [start, bytes] : chunks_) {
            const std::uint64_t end = start + bytes.size();
            if (end <= offset || start >= request_end) {
                continue;
            }
            const auto copy_begin = std::max(start, offset);
            const auto copy_end = std::min(end, request_end);
            const auto source = static_cast<std::size_t>(copy_begin - start);
            const auto target = static_cast<std::size_t>(copy_begin - offset);
            const auto count = static_cast<std::size_t>(copy_end - copy_begin);
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(source), count,
                        out.begin() + static_cast<std::ptrdiff_t>(target));
        }
        return true;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        if (offset > size_ || in.size() > size_ - offset) {
            return false;
        }
        chunks_[offset] = std::vector<std::byte>(in.begin(), in.end());
        ++write_calls;
        return true;
    }

    bool flush() override
    {
        ++flush_calls;
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

    std::size_t write_calls{};
    std::size_t flush_calls{};

private:
    std::uint64_t size_{};
    std::map<std::uint64_t, std::vector<std::byte>> chunks_;
};

void write_iso_record(std::span<std::byte> target, std::uint32_t extent_lba,
                      std::uint32_t data_bytes, std::uint8_t flags,
                      std::string_view identifier)
{
    const std::size_t padding = identifier.size() % 2 == 0 ? 1 : 0;
    const std::size_t length = 33 + identifier.size() + padding;
    target[0] = static_cast<std::byte>(length);
    store_u32_both(target.data() + 2, extent_lba);
    store_u32_both(target.data() + 10, data_bytes);
    target[25] = static_cast<std::byte>(flags);
    target[28] = std::byte{1};
    target[31] = std::byte{1};
    target[32] = static_cast<std::byte>(identifier.size());
    std::memcpy(target.data() + 33, identifier.data(), identifier.size());
}

MemoryIso make_game_iso()
{
    constexpr std::uint32_t root_lba = 20;
    constexpr std::uint32_t cnf_lba = 21;
    constexpr std::size_t sectors = 24;
    constexpr std::string_view cnf = "BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.00\r\n";
    MemoryIso image(sectors * 2048);
    for (std::size_t i = 0; i < image.bytes().size(); ++i) {
        image.bytes()[i] = static_cast<std::byte>((i * 23U + 11U) & 0xFFU);
    }

    auto pvd = image.bytes().subspan(16 * 2048, 2048);
    std::fill(pvd.begin(), pvd.end(), std::byte{0});
    pvd[0] = std::byte{1};
    std::memcpy(pvd.data() + 1, "CD001", 5);
    pvd[6] = std::byte{1};
    const std::string root_id(1, '\0');
    write_iso_record(pvd.subspan(156), root_lba, 2048, 0x02, root_id);

    auto root = image.bytes().subspan(root_lba * 2048, 2048);
    std::fill(root.begin(), root.end(), std::byte{0});
    const std::string dot(1, '\0');
    write_iso_record(root, root_lba, 2048, 0x02, dot);
    const auto used = std::to_integer<unsigned char>(root[0]);
    write_iso_record(root.subspan(used), cnf_lba, static_cast<std::uint32_t>(cnf.size()), 0,
                     "SYSTEM.CNF;1");
    std::memcpy(image.bytes().data() + cnf_lba * 2048, cnf.data(), cnf.size());
    return image;
}

ps2hdd::apa::Header make_apa_header(const char* id, std::uint16_t type,
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

void put_pfs_dentry(std::span<std::byte> sector, std::size_t offset,
                    std::uint32_t inode, std::string_view name,
                    std::uint16_t mode, std::uint16_t allocated)
{
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset), &inode, sizeof(inode));
    sector[offset + 4] = std::byte{0};
    sector[offset + 5] = static_cast<std::byte>(name.size());
    const auto raw = static_cast<std::uint16_t>(mode | allocated);
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 6), &raw, sizeof(raw));
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 8), name.data(), name.size());
}

struct DeployDiskFixture {
    static constexpr std::uint32_t chunk = ps2hdd::apa::kAllocationChunkSectors;
    static constexpr std::uint32_t pfs_lba = chunk;
    static constexpr std::uint32_t pfs_sectors = chunk;
    static constexpr std::uint32_t zone_size = 8192;
    static constexpr std::uint32_t sectors_per_zone = zone_size / ps2hdd::apa::kSectorSize;
    static constexpr std::uint32_t root_inode_zone = 100;
    static constexpr std::uint32_t root_data_zone = 101;
    static constexpr std::uint32_t bitmap_sector = 0x2010;

    DeployDiskFixture()
        : disk(static_cast<std::uint64_t>(8) * chunk * ps2hdd::apa::kSectorSize)
    {
        auto mbr = make_apa_header("__mbr", ps2hdd::apa::kTypeMbr, 0, chunk, pfs_lba, pfs_lba);
        constexpr char sony[] = "Sony Computer Entertainment Inc.";
        std::memcpy(mbr.mbr.magic, sony, sizeof(sony) - 1);
        mbr.checksum = ps2hdd::apa::checksum(mbr);
        const auto pfs = make_apa_header("+OPL", ps2hdd::apa::kTypePfs,
                                         pfs_lba, pfs_sectors, 0, 0);
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
        put_pfs_dentry(root_dir, 0, root_inode_zone, ".", ps2hdd::pfs::kModeDirectory, 12);
        put_pfs_dentry(root_dir, 12, root_inode_zone, "..", ps2hdd::pfs::kModeDirectory, 500);
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

        std::array<std::byte, ps2hdd::pfs::kMetadataSize> second_bitmap{};
        disk.put(base + static_cast<std::uint64_t>(bitmap_sector + 2U) *
                            ps2hdd::apa::kSectorSize,
                 second_bitmap);

        ps2hdd::apa::Reader reader(disk);
        scan = reader.scan();
        check(scan.ok() && scan.partitions.size() == 2, "deploy fixture APA scan");
    }

    SparseOverlayDisk disk;
    ps2hdd::apa::ScanResult scan;
};

std::filesystem::path temp_root()
{
    const auto path = std::filesystem::temp_directory_path() / "ps2-driveforge-image-deploy-tests";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path);
    return path;
}

void write_host(const std::filesystem::path& path, std::string_view data)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(static_cast<bool>(output), "open staged deployment asset");
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    check(static_cast<bool>(output), "write staged deployment asset");
}

ps2hdd::opl::FetchedAsset tar_asset(ps2hdd::opl::AssetKind kind,
                                    std::string target,
                                    std::string member,
                                    const std::filesystem::path& staged)
{
    ps2hdd::opl::FetchedAsset asset;
    asset.kind = kind;
    asset.placement = ps2hdd::opl::Placement::opl_data;
    asset.provider = "deploy-integration";
    asset.source_url = "https://example.invalid/deploy";
    asset.target_path = std::move(target);
    asset.archive_member = std::move(member);
    asset.staged_path = staged;
    return asset;
}

std::optional<std::vector<std::byte>> tar_member(std::span<const std::byte> archive,
                                                 std::string_view wanted)
{
    constexpr std::size_t record = 512;
    std::size_t cursor = 0;
    while (cursor + record <= archive.size()) {
        const auto header = archive.subspan(cursor, record);
        if (std::all_of(header.begin(), header.end(), [](std::byte value) {
                return value == std::byte{0};
            })) {
            return std::nullopt;
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
            if (ch < static_cast<unsigned char>('0') || ch > static_cast<unsigned char>('7')) {
                return std::nullopt;
            }
            size = size * 8U + (ch - static_cast<unsigned char>('0'));
        }
        if (size > std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        const auto payload = static_cast<std::size_t>(size);
        const auto padded = ((payload + record - 1U) / record) * record;
        if (cursor + record > archive.size() || padded > archive.size() - (cursor + record)) {
            return std::nullopt;
        }
        if (name == wanted) {
            return std::vector<std::byte>(
                archive.begin() + static_cast<std::ptrdiff_t>(cursor + record),
                archive.begin() + static_cast<std::ptrdiff_t>(cursor + record + payload));
        }
        cursor += record + padded;
    }
    return std::nullopt;
}

std::vector<std::byte> text_bytes(std::string_view text)
{
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

constexpr std::uint32_t kChunk = ps2hdd::apa::kAllocationChunkSectors;

// Turns the fixture's sector 0 into an APA-Jail type A2 hybrid whose APA
// region ends after `apa_chunks` allocation chunks, exactly like the PSBBN
// installer (sfdisk table + signature, then the APA checksum is fixed).
void make_jail(DeployDiskFixture& fixture, std::uint32_t apa_chunks)
{
    ps2hdd::apa::Header mbr{};
    check(fixture.disk.read(0, std::as_writable_bytes(std::span{&mbr, 1})), "read __mbr");
    auto* raw = reinterpret_cast<unsigned char*>(&mbr);
    std::memcpy(raw + ps2hdd::apa::kJailSignatureOffset, "APAJ-A2\0", 8);
    const auto entry = [&](std::size_t index, std::uint8_t type, std::uint32_t start, std::uint32_t sectors) {
        auto* e = raw + ps2hdd::apa::kMbrTableOffset + index * 16U;
        e[4] = type;
        std::memcpy(e + 8, &start, 4);
        std::memcpy(e + 12, &sectors, 4);
    };
    const std::uint32_t apa_end = apa_chunks * kChunk;
    const std::uint32_t total = 8U * kChunk;
    entry(0, 0x17, 2048, apa_end - 2048);
    entry(1, 0x17, apa_end, 65536);                      // 32 MiB RECOVERY
    entry(2, 0x07, apa_end + 65536, total - apa_end - 65536); // exFAT "OPL"
    raw[0x1FE] = 0x55;
    raw[0x1FF] = 0xAA;
    mbr.checksum = ps2hdd::apa::checksum(mbr);
    fixture.disk.put_object(0, mbr);
}

std::array<unsigned char, 0x120> sector0_pc_bytes(ps2hdd::BlockDevice& disk)
{
    std::array<std::byte, 512> sector{};
    check(disk.read(0, sector), "read sector 0");
    std::array<unsigned char, 0x120> out{};
    // 0xE0..0x1FF: signature, padding, MBR table and boot signature.
    std::memcpy(out.data(), sector.data() + 0xE0, out.size());
    return out;
}

void detects_type_a2_jail()
{
    DeployDiskFixture fixture;
    make_jail(fixture, 4);
    ps2hdd::apa::Reader reader(fixture.disk);
    const auto scan = reader.scan();
    check(scan.ok(), "jail fixture scans clean");
    check(scan.jail.has_value(), "APA-Jail detected");
    check(scan.jail->signature == "APAJ-A2", "type A2 signature");
    check(scan.jail->apa_limit_lba == 4ULL * kChunk, "APA boundary is the end of the 0x17 region");
    check(scan.jail->exfat && scan.jail->exfat->start_lba == 4U * kChunk + 65536U, "exFAT partition located");
    check(scan.jail->recovery.has_value(), "recovery partition located");

    DeployDiskFixture plain;
    check(!ps2hdd::apa::Reader(plain.disk).scan().jail, "ordinary APA disks are not hybrids");
}

void allocation_is_confined_to_the_jail()
{
    DeployDiskFixture fixture;
    make_jail(fixture, 4); // chunks 0..3 are APA: __mbr, +OPL, two free chunks (256 MiB)
    ps2hdd::apa::Reader reader(fixture.disk);
    const auto scan = reader.scan();

    const auto fits = ps2hdd::apa::plan_hdl_allocation(scan, fixture.disk.size_bytes(), 200ULL * 1024ULL * 1024ULL);
    check(fits.ok, "a game that fits inside the jail is planned");
    for (const auto& extent : fits.extents) {
        check(static_cast<std::uint64_t>(extent.start_lba) + extent.length_sectors <= scan.jail->apa_limit_lba,
              "every planned extent stays inside the APA region");
    }
    const auto too_big = ps2hdd::apa::plan_hdl_allocation(scan, fixture.disk.size_bytes(), 600ULL * 1024ULL * 1024ULL);
    check(!too_big.ok, "a game that only fits by spilling into exFAT is refused");

    DeployDiskFixture plain;
    const auto plain_scan = ps2hdd::apa::Reader(plain.disk).scan();
    check(ps2hdd::apa::plan_hdl_allocation(plain_scan, plain.disk.size_bytes(), 600ULL * 1024ULL * 1024ULL).ok,
          "the same game fits on a non-hybrid disk (control)");
}

void install_preserves_the_pc_partition_table()
{
    DeployDiskFixture fixture;
    make_jail(fixture, 4);
    const auto before = sector0_pc_bytes(fixture.disk);

    auto iso = make_game_iso();
    ps2hdd::ImageGameDeployOptions options;
    options.hdl.title = "Jail Quest";
    options.hdl.media = ps2hdd::hdl::MediaType::dvd;
    options.hdl.copy_buffer_bytes = 16U * 2048U;
    const auto installed = ps2hdd::deploy_game_to_image(fixture.disk, iso, {}, options);
    check(installed.ok, "install into an APA-Jail image");

    check(sector0_pc_bytes(fixture.disk) == before, "signature and MBR table survive APA header rewrites");
    const auto scan = ps2hdd::apa::Reader(fixture.disk).scan();
    check(scan.ok() && scan.jail && scan.jail->signature == "APAJ-A2", "hybrid still valid after install");
    for (const auto& partition : scan.partitions) {
        check(static_cast<std::uint64_t>(partition.start_lba) + partition.length_sectors <= scan.jail->apa_limit_lba,
              "installed partitions stay inside the jail");
    }
}

void boundary_violations_fail_closed()
{
    DeployDiskFixture fixture;
    make_jail(fixture, 1); // APA region ends before the existing +OPL partition
    const auto scan = ps2hdd::apa::Reader(fixture.disk).scan();
    check(!scan.ok(), "an APA partition crossing the boundary is a fatal scan error");
    check(std::any_of(scan.issues.begin(), scan.issues.end(), [](const auto& issue) {
              return issue.message.find("crosses the APA-Jail boundary") != std::string::npos;
          }),
          "the error names the problem");
    check(!ps2hdd::apa::plan_hdl_allocation(scan, fixture.disk.size_bytes(), 1024).ok,
          "no allocation on a corrupt hybrid");
}

} // namespace

int main()
{
    try {
        detects_type_a2_jail();
        allocation_is_confined_to_the_jail();
        install_preserves_the_pc_partition_table();
        boundary_violations_fail_closed();
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "APA-Jail tests passed\n";
    return 0;
}
