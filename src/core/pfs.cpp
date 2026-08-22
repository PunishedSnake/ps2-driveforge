#include "ps2hdd/pfs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

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

std::uint16_t load_u16(const std::byte* ptr)
{
    std::uint16_t value{};
    std::memcpy(&value, ptr, sizeof(value));
    return value;
}

std::uint32_t load_u32(const std::byte* ptr)
{
    std::uint32_t value{};
    std::memcpy(&value, ptr, sizeof(value));
    return value;
}

template <typename T>
std::uint32_t metadata_checksum(const T& metadata) noexcept
{
    static_assert(sizeof(T) == kMetadataSize);

    // PFS metadata uses the same broad checksum pattern as the upstream
    // implementation: the first u32 stores the checksum and the remaining
    // 1020 bytes are summed as 32-bit words. memcpy keeps this safe even though
    // the on-disk structure is packed and may not have natural u32 alignment.
    const auto* bytes = reinterpret_cast<const std::byte*>(&metadata);
    std::uint32_t sum = 0;
    for (std::size_t offset = sizeof(std::uint32_t); offset < kMetadataSize;
         offset += sizeof(std::uint32_t)) {
        std::uint32_t word{};
        std::memcpy(&word, bytes + static_cast<std::ptrdiff_t>(offset), sizeof(word));
        sum += word;
    }
    return sum;
}

} // namespace

bool valid_zone_size(std::uint32_t zone_size) noexcept
{
    return zone_size >= 2U * 1024U && zone_size <= 128U * 1024U &&
           (zone_size & (zone_size - 1U)) == 0;
}

std::uint32_t inode_checksum(const Inode& inode) noexcept
{
    return metadata_checksum(inode);
}

std::uint32_t segment_checksum(const SegmentDescriptor& descriptor) noexcept
{
    return metadata_checksum(descriptor);
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

Reader::Reader(ApaVolume& volume)
    : volume_(volume), probe_(probe(volume))
{
    if (!probe_.valid && !probe_.errors.empty()) {
        last_error_ = probe_.errors.front();
    }
}

unsigned Reader::inode_scale() const noexcept
{
    // This is metadata addressing, not file-data addressing. PFS metadata is
    // fixed at 1024 bytes while BlockInfo.number is expressed in the filesystem
    // zone scale. PS2SDK shifts the metadata block number by log2(zone/1024).
    // Do not replace this with `number * sectors_per_zone` -- that is correct for
    // payload extents and wrong for inode/SEGI metadata.
    unsigned scale = 0;
    std::uint32_t size = static_cast<std::uint32_t>(kMetadataSize);
    while (size < probe_.super.zone_size) {
        size <<= 1U;
        ++scale;
    }
    return scale;
}

void Reader::fail(std::string message)
{
    last_error_ = std::move(message);
}

std::optional<Node> Reader::root()
{
    if (!valid()) {
        fail("PFS filesystem is not valid");
        return std::nullopt;
    }
    return read_inode(probe_.super.root);
}

std::optional<Node> Reader::read_inode(BlockInfo location)
{
    last_error_.clear();
    if (!valid()) {
        fail("PFS filesystem is not valid");
        return std::nullopt;
    }
    if (location.subpart > probe_.super.num_subs || location.subpart >= volume_.extent_count()) {
        fail("PFS inode points to a missing sub-partition");
        return std::nullopt;
    }

    const auto scale = inode_scale();
    if (location.number > (std::numeric_limits<std::uint32_t>::max() >> scale)) {
        fail("PFS inode block address overflows");
        return std::nullopt;
    }

    // `location.number` is not a disk sector. Convert the PFS metadata address
    // to a 1024-byte metadata-block index first, then to 512-byte sectors.
    // ApaVolume applies the selected main/sub-partition physical start LBA later.
    const std::uint32_t metadata_block = location.number << scale;
    constexpr std::uint32_t sectors_per_metadata =
        static_cast<std::uint32_t>(kMetadataSize / apa::kSectorSize);
    const std::uint64_t sector64 = static_cast<std::uint64_t>(metadata_block) * sectors_per_metadata;
    if (sector64 > std::numeric_limits<std::uint32_t>::max()) {
        fail("PFS inode sector address overflows");
        return std::nullopt;
    }

    std::array<std::byte, kMetadataSize> raw{};
    if (!volume_.read_sectors(location.subpart, static_cast<std::uint32_t>(sector64),
                              sectors_per_metadata, raw)) {
        fail("Could not read PFS inode metadata");
        return std::nullopt;
    }

    Node node;
    node.location = location;
    std::memcpy(&node.inode, raw.data(), sizeof(node.inode));

    if (node.inode.magic != kSegdMagic) {
        fail("PFS inode has invalid SEGD magic");
        return std::nullopt;
    }
    if (inode_checksum(node.inode) != node.inode.checksum) {
        fail("PFS inode checksum mismatch");
        return std::nullopt;
    }
    if (node.inode.number_data == 0) {
        fail("PFS inode has no block descriptors");
        return std::nullopt;
    }
    if (node.inode.number_data > kInodeMaxBlocks && node.inode.next_segment.number == 0) {
        fail("PFS inode references indirect data but has no SEGI chain");
        return std::nullopt;
    }

    return node;
}

std::optional<SegmentDescriptor> Reader::read_segment_descriptor(BlockInfo location)
{
    if (location.number == 0) {
        fail("PFS SEGI chain contains a null descriptor pointer");
        return std::nullopt;
    }
    if (location.subpart > probe_.super.num_subs || location.subpart >= volume_.extent_count()) {
        fail("PFS SEGI descriptor points to a missing sub-partition");
        return std::nullopt;
    }

    const auto scale = inode_scale();
    if (location.number > (std::numeric_limits<std::uint32_t>::max() >> scale)) {
        fail("PFS SEGI block address overflows");
        return std::nullopt;
    }
    const std::uint32_t metadata_block = location.number << scale;
    constexpr std::uint32_t sectors_per_metadata =
        static_cast<std::uint32_t>(kMetadataSize / apa::kSectorSize);
    const std::uint64_t sector64 = static_cast<std::uint64_t>(metadata_block) * sectors_per_metadata;
    if (sector64 > std::numeric_limits<std::uint32_t>::max()) {
        fail("PFS SEGI sector address overflows");
        return std::nullopt;
    }

    std::array<std::byte, kMetadataSize> raw{};
    if (!volume_.read_sectors(location.subpart, static_cast<std::uint32_t>(sector64),
                              sectors_per_metadata, raw)) {
        fail("Could not read PFS SEGI metadata");
        return std::nullopt;
    }

    SegmentDescriptor descriptor{};
    std::memcpy(&descriptor, raw.data(), sizeof(descriptor));
    if (descriptor.magic != kSegiMagic) {
        fail("PFS indirect descriptor has invalid SEGI magic");
        return std::nullopt;
    }
    if (segment_checksum(descriptor) != descriptor.checksum) {
        fail("PFS indirect descriptor checksum mismatch");
        return std::nullopt;
    }
    return descriptor;
}

bool Reader::read_zone_bytes(const BlockInfo& block, std::uint64_t zone_offset,
                             std::span<std::byte> out)
{
    if (block.subpart > probe_.super.num_subs || block.subpart >= volume_.extent_count()) {
        fail("PFS data block points to a missing sub-partition");
        return false;
    }

    const std::uint64_t segment_bytes =
        static_cast<std::uint64_t>(block.count) * probe_.super.zone_size;
    if (zone_offset > segment_bytes || out.size() > segment_bytes - zone_offset) {
        fail("PFS read exceeds block descriptor extent");
        return false;
    }

    // File-data BlockInfo.number is a zone number. This conversion is different
    // from inode metadata addressing above. After zone -> sector conversion,
    // ApaVolume still has to select the physical APA main/sub extent.
    const std::uint32_t sectors_per_zone = probe_.super.zone_size / apa::kSectorSize;
    const std::uint64_t base_sector = static_cast<std::uint64_t>(block.number) * sectors_per_zone;
    std::uint64_t byte_position = zone_offset;
    std::size_t done = 0;

    while (done < out.size()) {
        const std::uint64_t relative_sector = byte_position / apa::kSectorSize;
        const std::size_t in_sector = static_cast<std::size_t>(byte_position % apa::kSectorSize);
        const std::size_t remaining = out.size() - done;

        if (in_sector == 0 && remaining >= apa::kSectorSize) {
            const std::size_t whole_sectors = remaining / apa::kSectorSize;

            // 128 sectors = 64 KiB. This is a conservative current I/O tuning
            // value, not a PFS format limit. Keep it documented/benchmarked in
            // docs/performance.md before changing it.
            const std::size_t batch = std::min<std::size_t>(whole_sectors, 128);
            const std::uint64_t sector64 = base_sector + relative_sector;
            if (sector64 > std::numeric_limits<std::uint32_t>::max()) {
                fail("PFS data sector address overflows");
                return false;
            }
            const auto bytes = batch * apa::kSectorSize;
            if (!volume_.read_sectors(block.subpart, static_cast<std::uint32_t>(sector64),
                                      static_cast<std::uint32_t>(batch), out.subspan(done, bytes))) {
                fail("Could not read PFS data sectors");
                return false;
            }
            done += bytes;
            byte_position += bytes;
            continue;
        }

        // Unaligned edges still need a full 512-byte disk-sector read. This is
        // what lets Reader::read satisfy Windows-style arbitrary byte ranges
        // without weakening the lower BlockDevice/ApaVolume sector contract.
        std::array<std::byte, apa::kSectorSize> sector{};
        const std::uint64_t sector64 = base_sector + relative_sector;
        if (sector64 > std::numeric_limits<std::uint32_t>::max() ||
            !volume_.read_sectors(block.subpart, static_cast<std::uint32_t>(sector64), 1, sector)) {
            fail("Could not read PFS data sector");
            return false;
        }
        const std::size_t take = std::min(remaining, apa::kSectorSize - in_sector);
        std::copy_n(sector.begin() + static_cast<std::ptrdiff_t>(in_sector), take,
                    out.begin() + static_cast<std::ptrdiff_t>(done));
        done += take;
        byte_position += take;
    }

    return true;
}

bool Reader::read(const Node& node, std::uint64_t offset, std::span<std::byte> out)
{
    last_error_.clear();
    if (!valid()) {
        fail("PFS filesystem is not valid");
        return false;
    }
    if (offset > node.inode.size || out.size() > node.inode.size - offset) {
        fail("PFS read is outside file bounds");
        return false;
    }
    if (out.empty()) {
        return true;
    }

    struct DataRun {
        BlockInfo block{};
        std::uint64_t logical_start{};
        std::uint64_t logical_bytes{};
    };

    const std::uint64_t request_end = offset + out.size();
    std::vector<DataRun> runs;
    std::uint64_t logical = 0;
    std::optional<SegmentDescriptor> indirect;
    BlockInfo next_segment = node.inode.next_segment;
    std::size_t segi_loaded = 0;

    // First walk only as far as this request needs. Adjacent PFS payload
    // descriptors in the same APA subpart are collapsed into a single logical
    // run. This removes artificial host-I/O boundaries created by inode metadata
    // without ever crossing an APA main/sub boundary or a physical zone gap.
    for (std::size_t global = 1;
         global < node.inode.number_data && logical < request_end;
         ++global) {
        BlockInfo block{};

        if (global < kInodeMaxBlocks) {
            block = node.inode.data[global];
        } else {
            const std::size_t local = (global - kInodeMaxBlocks) % kIndirectMaxBlocks;
            if (local == 0) {
                auto loaded = read_segment_descriptor(next_segment);
                if (!loaded) {
                    return false;
                }
                indirect = *loaded;
                next_segment = indirect->next_segment;
                ++segi_loaded;
                continue;
            }
            if (!indirect) {
                fail("PFS indirect data encountered before a SEGI descriptor");
                return false;
            }
            block = indirect->data[local];
        }

        if (block.count == 0) {
            fail("PFS contains an empty block descriptor inside the used descriptor range");
            return false;
        }
        const std::uint64_t segment_size =
            static_cast<std::uint64_t>(block.count) * probe_.super.zone_size;
        if (logical > std::numeric_limits<std::uint64_t>::max() - segment_size) {
            fail("PFS logical file extent overflows");
            return false;
        }
        const std::uint64_t segment_end = logical + segment_size;

        if (segment_end > offset && logical < request_end) {
            bool merged = false;
            if (!runs.empty()) {
                auto& previous = runs.back();
                const std::uint64_t expected_zone =
                    static_cast<std::uint64_t>(previous.block.number) + previous.block.count;
                const std::uint64_t combined_count =
                    static_cast<std::uint64_t>(previous.block.count) + block.count;
                if (previous.block.subpart == block.subpart &&
                    expected_zone == block.number &&
                    previous.logical_start + previous.logical_bytes == logical &&
                    combined_count <= std::numeric_limits<std::uint16_t>::max()) {
                    previous.block.count = static_cast<std::uint16_t>(combined_count);
                    previous.logical_bytes += segment_size;
                    merged = true;
                }
            }
            if (!merged) {
                runs.push_back({block, logical, segment_size});
            }
        }
        logical = segment_end;
    }

    std::uint64_t request_position = offset;
    std::size_t written = 0;
    for (const auto& run : runs) {
        if (written == out.size()) {
            break;
        }
        const std::uint64_t run_end = run.logical_start + run.logical_bytes;
        if (request_position >= run_end) {
            continue;
        }

        const std::uint64_t within =
            request_position > run.logical_start ? request_position - run.logical_start : 0;
        const std::uint64_t available = run.logical_bytes - within;
        const std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>(available, out.size() - written));
        if (!read_zone_bytes(run.block, within, out.subspan(written, take))) {
            return false;
        }
        written += take;
        request_position += take;
    }

    if (written != out.size()) {
        fail("PFS inode does not describe enough data for the requested read");
        return false;
    }
    if (node.inode.number_segdesg != 0 && segi_loaded > node.inode.number_segdesg) {
        fail("PFS SEGI chain is longer than inode metadata reports");
        return false;
    }
    return true;
}

std::vector<DirectoryEntry> Reader::list_directory(const Node& directory,
                                                   bool include_dot_entries)
{
    last_error_.clear();
    std::vector<DirectoryEntry> entries;
    if ((directory.inode.mode & kModeMask) != kModeDirectory) {
        fail("PFS node is not a directory");
        return entries;
    }
    if (directory.inode.size > 64ULL * 1024ULL * 1024ULL) {
        fail("PFS directory size is implausibly large");
        return entries;
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(directory.inode.size));
    if (!read(directory, 0, bytes)) {
        return entries;
    }

    std::size_t position = 0;
    while (position < bytes.size()) {
        // PFS directory entries are variable length but may not cross a physical
        // 512-byte sector boundary. Preserve this boundary check even if the
        // parser is later optimized to avoid materializing the whole directory.
        const std::size_t sector_start = (position / apa::kSectorSize) * apa::kSectorSize;
        const std::size_t sector_end = std::min(sector_start + apa::kSectorSize, bytes.size());
        if (position + 8 > sector_end) {
            position = sector_end;
            continue;
        }

        const auto* ptr = bytes.data() + static_cast<std::ptrdiff_t>(position);
        const std::uint32_t inode_number = load_u32(ptr);
        const std::uint8_t subpart = static_cast<std::uint8_t>(ptr[4]);
        const std::uint8_t path_len = static_cast<std::uint8_t>(ptr[5]);
        const std::uint16_t raw_len = load_u16(ptr + 6);
        const std::uint16_t allocated = raw_len & 0x0FFFU;
        const std::uint16_t mode = raw_len & kModeMask;

        if (allocated < 8 || (allocated & 3U) != 0 || position + allocated > sector_end) {
            fail("Malformed PFS directory entry allocation length");
            entries.clear();
            return entries;
        }
        if (path_len > allocated - 8) {
            fail("Malformed PFS directory entry name length");
            entries.clear();
            return entries;
        }

        if (inode_number != 0 && path_len != 0) {
            std::string name(reinterpret_cast<const char*>(ptr + 8), path_len);
            if (include_dot_entries || (name != "." && name != "..")) {
                DirectoryEntry entry;
                entry.name = std::move(name);
                entry.inode.number = inode_number;
                entry.inode.subpart = subpart;
                entry.inode.count = 1;
                entry.mode = mode;
                entries.emplace_back(std::move(entry));
            }
        }
        position += allocated;
    }

    return entries;
}

std::optional<Node> Reader::resolve(std::string_view path)
{
    last_error_.clear();
    auto current = root();
    if (!current) {
        return std::nullopt;
    }

    // Path traversal is explicit Reader state, not a process-global current
    // directory. This is an intentional host-API difference from shell/iomanX
    // style frontends and lets GUI/Dokany callers resolve independent paths.
    std::size_t position = 0;
    while (position < path.size()) {
        while (position < path.size() && (path[position] == '/' || path[position] == '\\')) {
            ++position;
        }
        if (position == path.size()) {
            break;
        }
        const std::size_t start = position;
        while (position < path.size() && path[position] != '/' && path[position] != '\\') {
            ++position;
        }
        const std::string_view component = path.substr(start, position - start);
        if (component.empty() || component == ".") {
            continue;
        }

        const auto entries = list_directory(*current, true);
        if (!last_error_.empty()) {
            return std::nullopt;
        }
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [component](const DirectoryEntry& e) { return e.name == component; });
        if (it == entries.end()) {
            fail("PFS path component not found: " + std::string(component));
            return std::nullopt;
        }
        current = read_inode(it->inode);
        if (!current) {
            return std::nullopt;
        }
    }

    return current;
}

} // namespace ps2hdd::pfs
