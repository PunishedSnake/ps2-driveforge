#include "ps2hdd/pfs.hpp"

#include <array>
#include <bit>
#include <cstring>

namespace ps2hdd::pfs {

static_assert(std::endian::native == std::endian::little,
              "PS2 HDD on-disk structures are currently decoded on little-endian hosts only");
namespace {

bool read_super(ApaVolume& volume, std::uint32_t sector, SuperBlock& out)
{
    std::array<std::byte, apa::kSectorSize> buffer{};
    if (!volume.read_sectors(0, sector, 1, buffer)) {
        return false;
    }
    std::memcpy(&out, buffer.data(), sizeof(out));
    return true;
}

} // namespace

bool valid_zone_size(std::uint32_t zone_size) noexcept
{
    return zone_size >= 2U * 1024U && zone_size <= 128U * 1024U &&
           (zone_size & (zone_size - 1U)) == 0;
}

ProbeResult probe(ApaVolume& volume)
{
    ProbeResult result;

    if (volume.partition().type != apa::kTypePfs) {
        result.errors.emplace_back("APA partition is not marked as PFS");
        return result;
    }
    if (volume.partition().is_sub()) {
        result.errors.emplace_back("A PFS filesystem must be probed from its main APA partition");
        return result;
    }

    if (!read_super(volume, kSuperSector, result.super)) {
        result.errors.emplace_back("Could not read PFS superblock at sector 8192");
        return result;
    }
    if (result.super.magic != kSuperMagic) {
        result.errors.emplace_back("PFS superblock magic is invalid");
        return result;
    }
    if (result.super.version > kFormatVersion) {
        result.errors.emplace_back("PFS format version is newer than supported version 3");
        return result;
    }
    if (!valid_zone_size(result.super.zone_size)) {
        result.errors.emplace_back("PFS zone size is invalid");
        return result;
    }

    const std::size_t available_subs = volume.extent_count() - 1;
    if (result.super.num_subs > available_subs) {
        result.errors.emplace_back("PFS expects more APA sub-partitions than are attached");
        return result;
    }
    if (result.super.num_subs < available_subs) {
        result.warnings.emplace_back("APA has additional sub-partitions not registered by this PFS superblock");
    }

    if (result.super.root.subpart > result.super.num_subs) {
        result.errors.emplace_back("PFS root inode points to a missing sub-partition");
        return result;
    }
    if (result.super.log.subpart > result.super.num_subs) {
        result.errors.emplace_back("PFS journal points to a missing sub-partition");
        return result;
    }

    SuperBlock backup{};
    if (!read_super(volume, kSuperBackupSector, backup)) {
        result.warnings.emplace_back("Could not read PFS backup superblock at sector 8193");
    } else {
        result.backup_matches = std::memcmp(&result.super, &backup, sizeof(SuperBlock)) == 0;
        if (!result.backup_matches) {
            result.warnings.emplace_back("Primary and backup PFS superblocks differ");
        }
    }

    if ((result.super.fsck_stat & kFsckWriteError) != 0) {
        result.warnings.emplace_back("PFS fsck status contains WRITE_ERROR");
    }
    if ((result.super.fsck_stat & kFsckErrorsFixed) != 0) {
        result.warnings.emplace_back("PFS fsck status reports previously fixed errors");
    }

    result.valid = true;
    return result;
}

} // namespace ps2hdd::pfs
