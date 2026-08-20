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

    void put_header(std::uint32_t lba, const Header& h)
    {
        const auto bytes = std::as_bytes(std::span{&h, 1});
        put_bytes(static_cast<std::uint64_t>(lba) * ps2hdd::apa::kSectorSize, bytes);
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

} // namespace

int main()
{
    try {
        valid_chain_is_read();
        bad_checksum_is_rejected();
        cycle_is_detected();
        pfs_superblock_is_probed();
        std::cout << "All APA/PFS tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
