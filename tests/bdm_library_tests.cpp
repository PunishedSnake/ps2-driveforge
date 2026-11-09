// Frieren BDM (exFAT/USB/APA-Jail) library regressions. Fixture helpers are
// shared verbatim with image_game_deploy_tests.cpp.

#include "ps2hdd/image_game_deploy.hpp"
#include "ps2hdd/bdm_library.hpp"
#include "ps2hdd/http_client.hpp"

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
    const auto path = std::filesystem::temp_directory_path() / "ps2-driveforge-bdm-library-tests";
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path);
    return path;
}

class FakeHttp final : public ps2hdd::HttpClient {
public:
    ps2hdd::HttpResponse get(std::string_view url, std::size_t) override
    {
        urls.emplace_back(url);
        const std::string u(url);
        const auto body = [](std::string_view text) {
            const auto raw = std::as_bytes(std::span{text});
            return std::vector<std::byte>(raw.begin(), raw.end());
        };
        if (u.find(".cfg") != std::string::npos) return {200, body("Title=BDM Quest\n"), "text/plain", {}};
        if (u.find(".cht") != std::string::npos) return {200, body("\"BDM\"\nMastercode\n90000000 00000000\n"), "text/plain", {}};
        if (u.find("_ICO") != std::string::npos || u.find("_BG") != std::string::npos ||
            u.find("_SCR") != std::string::npos || u.find("_COV") != std::string::npos) {
            return {200, body("\x89PNG fake"), "image/png", {}};
        }
        return {404, {}, "text/plain", {}};
    }
    [[nodiscard]] bool requested(std::string_view needle) const
    {
        return std::any_of(urls.begin(), urls.end(), [needle](const auto& u) { return u.find(needle) != std::string::npos; });
    }
    std::vector<std::string> urls;
};

std::filesystem::path write_iso_file(const std::filesystem::path& dir)
{
    auto iso = make_game_iso();
    const auto path = dir / "BDM Quest (USA).iso";
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(iso.bytes().data()), static_cast<std::streamsize>(iso.bytes().size()));
    return path;
}

std::vector<std::byte> slurp(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>(in), {}};
    std::vector<std::byte> out(raw.size());
    std::memcpy(out.data(), raw.data(), raw.size());
    return out;
}

void layout_and_names()
{
    const auto root = temp_root();
    const auto first = ps2hdd::bdm::ensure_layout(root);
    check(first.ok && first.created.size() == ps2hdd::bdm::kLayoutDirectories.size(), "OPL BDM folders created");
    check(ps2hdd::bdm::ensure_layout(root).created.empty(), "layout is idempotent");
    check(!ps2hdd::bdm::ensure_layout(root / "missing").ok, "missing root refused");

    check(ps2hdd::bdm::game_file_name("SLUS_123.45", "Quest") == "SLUS_123.45.Quest.iso", "ID-prefixed name");
    check(ps2hdd::bdm::game_file_name("SLUS_123.45", "A:B/C?") == "SLUS_123.45.A_B_C_.iso", "FAT-unsafe chars replaced");
    const auto long_name = ps2hdd::bdm::game_file_name("SLUS_123.45", std::string(200, 'x'));
    check(long_name.size() == ps2hdd::bdm::kMaxGameFileName, "name capped at the OPL limit");
}

void install_and_scan()
{
    const auto root = temp_root();
    const auto source_dir = root / "src";
    std::filesystem::create_directories(source_dir);
    const auto library = root / "lib";
    std::filesystem::create_directories(library);
    const auto iso = write_iso_file(source_dir);

    std::uint64_t last = 0;
    const auto installed = ps2hdd::bdm::install_game(library, iso, {}, [&](std::uint64_t done, std::uint64_t) { last = done; });
    if (!installed.ok) std::cerr << installed.error << "\n";
    check(installed.ok, "install into a BDM library");
    check(installed.startup == "SLUS_123.45", "boot ID read from SYSTEM.CNF");
    check(installed.media == ps2hdd::hdl::MediaType::cd && installed.path.parent_path().filename() == "CD",
          "small images default to the CD folder");
    check(installed.path.filename() == "SLUS_123.45.BDM Quest (USA).iso", "ID-prefixed file name");
    check(slurp(installed.path) == slurp(iso) && last == installed.bytes, "byte exact copy with progress");
    check(!std::filesystem::exists(installed.path.string() + ".partial"), "no partial file left");
    if (installed.fragments.known) check(installed.fragments.extents >= 1, "fragment report is sane");

    const auto again = ps2hdd::bdm::install_game(library, iso);
    check(!again.ok && again.error.find("already contains") != std::string::npos, "duplicate game ID refused");

    ps2hdd::bdm::InstallOptions dvd;
    dvd.media = ps2hdd::hdl::MediaType::dvd;
    dvd.overwrite = true;
    dvd.title = "Forced DVD";
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = ps2hdd::bdm::install_game(library, iso, dvd, {}, stop.get_token());
    check(!cancelled.ok && cancelled.cancelled, "cancel reported");
    check(!std::filesystem::exists(library / "DVD" / "SLUS_123.45.Forced DVD.iso") &&
              !std::filesystem::exists(library / "DVD" / "SLUS_123.45.Forced DVD.iso.partial"),
          "cancelled install leaves nothing");

    std::ofstream(library / "DVD" / "SLES_500.02.Compressed.zso") << "ZISO";
    const auto games = ps2hdd::bdm::scan_library(library);
    check(games.size() == 2, "scan finds ISO and ZSO");
    check(games[0].startup == "SLUS_123.45" && games[0].cd, "ISO identified");
    check(games[1].startup == "SLES_500.02" && !games[1].cd, "ZSO identified from its ID prefix");
}

void asset_sync_fills_only_missing()
{
    const auto root = temp_root();
    const auto library = root / "lib";
    std::filesystem::create_directories(library / "src");
    const auto iso = write_iso_file(library / "src");
    check(ps2hdd::bdm::install_game(library, iso).ok, "install");
    std::ofstream(library / "ART" / "SLUS_123.45_COV.jpg") << "USER";

    FakeHttp http;
    const auto dry = ps2hdd::bdm::sync_assets(library, http, root / "cache", false);
    check(dry.ok && dry.games == 1 && dry.already_present == 1 && dry.staged > 0 && dry.written == 0,
          "dry run stages but writes nothing");
    check(!http.requested("_COV"), "existing user cover is never fetched");
    check(!std::filesystem::exists(library / "CFG" / "SLUS_123.45.cfg"), "dry run left the library untouched");

    FakeHttp apply_http;
    const auto applied = ps2hdd::bdm::sync_assets(library, apply_http, root / "cache", true);
    check(applied.ok && applied.written == applied.staged && applied.written > 0, "apply writes staged files");
    check(std::filesystem::exists(library / "CFG" / "SLUS_123.45.cfg") &&
              std::filesystem::exists(library / "CHT" / "SLUS_123.45.cht"),
          "CFG and CHT written into the BDM folders");
    check(slurp(library / "ART" / "SLUS_123.45_COV.jpg").size() == 4, "user cover untouched");

    FakeHttp third;
    const auto again = ps2hdd::bdm::sync_assets(library, third, root / "cache", true);
    check(again.ok && again.written == 0 && third.urls.empty(), "second sync is a no-op without network traffic");
}

} // namespace

int main()
{
    try {
        layout_and_names();
        install_and_scan();
        asset_sync_fills_only_missing();
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "BDM library tests passed\n";
    return 0;
}
