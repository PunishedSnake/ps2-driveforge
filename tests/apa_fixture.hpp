#pragma once

// Shared test fixture: a sparse APA disk image with a canonical __mbr, an
// optional structurally valid KELF bootstrap at 0x2000, and any number of
// empty-but-valid PFS partitions (one allocation chunk each) that the normal
// pfs::ImageWriter can populate. Used by boot-chain, migration and
// fault-injection tests so they all exercise the real readers and writers.

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_allocation.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/pfs_write.hpp"
#include "ps2hdd/writable_file_block_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fixture {

inline constexpr std::uint32_t kChunk = ps2hdd::apa::kAllocationChunkSectors;
inline constexpr std::uint32_t kBootSector = 0x2000;

inline void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

inline ps2hdd::apa::Header header(const char* id, std::uint16_t type, std::uint32_t start, std::uint32_t length,
                                  std::uint32_t prev, std::uint32_t next)
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

class Writer {
public:
    explicit Writer(const std::filesystem::path& path) : out_(path, std::ios::binary | std::ios::in | std::ios::out) {}
    void put(std::uint64_t offset, std::span<const std::byte> bytes)
    {
        out_.seekp(static_cast<std::streamoff>(offset));
        out_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    template <typename T> void put_object(std::uint64_t offset, const T& value)
    {
        put(offset, std::as_bytes(std::span{&value, 1}));
    }

private:
    std::fstream out_;
};

inline void put_dentry(std::span<std::byte> sector, std::size_t offset, std::uint32_t inode, std::string_view name,
                       std::uint16_t mode, std::uint16_t allocated)
{
    std::memcpy(sector.data() + offset, &inode, sizeof(inode));
    sector[offset + 4] = std::byte{0};
    sector[offset + 5] = static_cast<std::byte>(name.size());
    const auto raw = static_cast<std::uint16_t>(mode | allocated);
    std::memcpy(sector.data() + offset + 6, &raw, sizeof(raw));
    std::memcpy(sector.data() + offset + 8, name.data(), name.size());
}

// Empty PFS (root directory only) inside the partition starting at `lba`.
inline void format_pfs(Writer& disk, std::uint32_t lba)
{
    constexpr std::uint32_t zone_size = 8192;
    constexpr std::uint32_t sectors_per_zone = zone_size / ps2hdd::apa::kSectorSize;
    constexpr std::uint32_t root_inode_zone = 100;
    constexpr std::uint32_t root_data_zone = 101;
    constexpr std::uint32_t bitmap_sector = 0x2010;
    const auto base = static_cast<std::uint64_t>(lba) * ps2hdd::apa::kSectorSize;

    ps2hdd::pfs::SuperBlock super{};
    super.magic = ps2hdd::pfs::kSuperMagic;
    super.version = ps2hdd::pfs::kFormatVersion;
    super.zone_size = zone_size;
    super.root = {root_inode_zone, 0, 1};
    super.log = {50, 0, 1};
    std::array<std::byte, ps2hdd::apa::kSectorSize> super_sector{};
    std::memcpy(super_sector.data(), &super, sizeof(super));
    disk.put(base + std::uint64_t{ps2hdd::pfs::kSuperSector} * ps2hdd::apa::kSectorSize, super_sector);
    disk.put(base + std::uint64_t{ps2hdd::pfs::kSuperBackupSector} * ps2hdd::apa::kSectorSize, super_sector);

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
    disk.put_object(base + std::uint64_t{root_inode_zone} * zone_size, root);

    std::array<std::byte, ps2hdd::apa::kSectorSize> root_dir{};
    put_dentry(root_dir, 0, root_inode_zone, ".", ps2hdd::pfs::kModeDirectory, 12);
    put_dentry(root_dir, 12, root_inode_zone, "..", ps2hdd::pfs::kModeDirectory, 500);
    disk.put(base + std::uint64_t{root_data_zone} * zone_size, root_dir);

    std::array<std::byte, ps2hdd::pfs::kMetadataSize> bitmap{};
    const auto mark = [&](std::uint32_t zone) {
        bitmap[zone / 8U] = static_cast<std::byte>(std::to_integer<unsigned>(bitmap[zone / 8U]) | (1U << (zone % 8U)));
    };
    for (std::uint32_t zone = 0; zone <= 110; ++zone) mark(zone);
    mark(ps2hdd::pfs::kSuperSector / sectors_per_zone);
    mark(bitmap_sector / sectors_per_zone);
    disk.put(base + std::uint64_t{bitmap_sector} * ps2hdd::apa::kSectorSize, bitmap);
    std::array<std::byte, ps2hdd::pfs::kMetadataSize> second{};
    disk.put(base + std::uint64_t{bitmap_sector + 2U} * ps2hdd::apa::kSectorSize, second);
}

inline std::vector<std::byte> kelf_payload()
{
    std::vector<std::byte> payload(512, std::byte{0});
    const std::uint32_t elf_size = 8;
    const std::uint16_t header_size = 72;
    std::memcpy(payload.data() + 0x10, &elf_size, 4);
    std::memcpy(payload.data() + 0x14, &header_size, 2);
    for (std::size_t i = 0; i < elf_size; ++i) payload[header_size + i] = static_cast<std::byte>(0x30 + i);
    return payload;
}

struct Spec {
    std::vector<std::string> pfs;   // PFS partitions, in chain order
    std::uint32_t total_chunks{8};  // image size in allocation chunks
    bool kelf{true};                // publish a KELF bootstrap at 0x2000
    std::uint32_t empty_chunks{};   // trailing "__empty" partition (header-only on disk)
};

// Builds the image and returns the LBA of every partition (mbr first).
inline std::vector<std::uint32_t> build(const std::filesystem::path& path, const Spec& spec)
{
    { std::ofstream create(path, std::ios::binary | std::ios::trunc); }
    std::filesystem::resize_file(path, std::uint64_t{spec.total_chunks} * kChunk * ps2hdd::apa::kSectorSize);
    Writer disk(path);

    std::vector<std::uint32_t> lbas{0};
    for (std::size_t i = 0; i < spec.pfs.size(); ++i) lbas.push_back(static_cast<std::uint32_t>((i + 1) * kChunk));
    const bool empty = spec.empty_chunks != 0;
    if (empty) lbas.push_back(static_cast<std::uint32_t>((spec.pfs.size() + 1) * kChunk));
    require((lbas.back() / kChunk) + (empty ? spec.empty_chunks : 1) <= spec.total_chunks, "fixture fits");

    const auto count = lbas.size();
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t next = i + 1 < count ? lbas[i + 1] : 0;
        const std::uint32_t prev = i == 0 ? lbas.back() : lbas[i - 1];
        ps2hdd::apa::Header h;
        if (i == 0) {
            h = header("__mbr", ps2hdd::apa::kTypeMbr, 0, kChunk, count > 1 ? prev : 0, next);
            constexpr char sony[] = "Sony Computer Entertainment Inc.";
            std::memcpy(h.mbr.magic, sony, sizeof(sony) - 1);
            if (spec.kelf) {
                h.mbr.osd_start = kBootSector;
                h.mbr.osd_size = 1;
            }
            h.checksum = ps2hdd::apa::checksum(h);
        } else if (empty && i == count - 1) {
            h = header("__empty", 0, lbas[i], spec.empty_chunks * kChunk, prev, next);
        } else {
            h = header(spec.pfs[i - 1].c_str(), ps2hdd::apa::kTypePfs, lbas[i], kChunk, i == 1 ? 0 : prev, next);
            if (i == 1) {
                h.prev = 0;
                h.checksum = ps2hdd::apa::checksum(h);
            }
        }
        disk.put_object(std::uint64_t{lbas[i]} * ps2hdd::apa::kSectorSize, h);
        if (i != 0 && !(empty && i == count - 1)) format_pfs(disk, lbas[i]);
    }
    if (spec.kelf) disk.put(std::uint64_t{kBootSector} * ps2hdd::apa::kSectorSize, kelf_payload());
    return lbas;
}

// Writes files (creating parent directories) into a PFS partition.
inline void put_files(const std::filesystem::path& image, std::string_view partition,
                      const std::vector<std::pair<std::string, std::string>>& files)
{
    ps2hdd::WritableFileBlockDevice disk(image);
    require(disk.is_open(), "open fixture for writing");
    ps2hdd::apa::Reader reader(disk);
    const auto scan = reader.scan();
    require(scan.ok(), "fixture APA scan");
    const ps2hdd::apa::Partition* target = nullptr;
    for (const auto& p : scan.partitions)
        if (!p.is_sub() && p.id == partition) target = &p;
    require(target != nullptr, "fixture partition exists");
    ps2hdd::pfs::ImageWriter writer(disk, *target);
    require(writer.valid(), "fixture PFS valid");
    for (const auto& [path, text] : files) {
        const auto slash = path.find_last_of('/');
        if (slash != std::string::npos && slash != 0)
            require(writer.ensure_directory(path.substr(0, slash)).ok, "fixture mkdir");
        const auto bytes = std::as_bytes(std::span(text));
        const auto written = writer.write_file(path, bytes);
        if (!written.ok) throw std::runtime_error("fixture write " + path + ": " + written.error);
    }
}

} // namespace fixture
