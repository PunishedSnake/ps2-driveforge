#include "ps2hdd/apa.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_set>

namespace ps2hdd::apa {

static_assert(std::endian::native == std::endian::little,
              "PS2 HDD on-disk structures are currently decoded on little-endian hosts only");
namespace {

constexpr char kSonyMbrMagic[] = "Sony Computer Entertainment Inc.";

std::uint32_t load_u32_le(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

bool range_fits(std::uint64_t offset, std::uint64_t size, std::uint64_t device_size) noexcept
{
    // Written this way rather than `offset + size <= device_size` so damaged
    // metadata cannot wrap the addition and turn an out-of-range read into an
    // apparently valid one.
    return offset <= device_size && size <= device_size - offset;
}

bool sector_extent_fits(std::uint32_t start, std::uint32_t length,
                        std::uint64_t device_size) noexcept
{
    const std::uint64_t offset = static_cast<std::uint64_t>(start) * kSectorSize;
    const std::uint64_t bytes = static_cast<std::uint64_t>(length) * kSectorSize;
    return range_fits(offset, bytes, device_size);
}

} // namespace

bool ScanResult::ok() const noexcept
{
    return mbr_valid && std::none_of(issues.begin(), issues.end(), [](const Issue& issue) {
        return issue.severity == IssueSeverity::error;
    });
}

std::uint32_t checksum(const Header& header) noexcept
{
    // APA follows the PS2SDK/libapa rule: view the 1024-byte header as 256
    // little-endian u32 words and sum words 1..255. Word 0 is the stored
    // checksum and is deliberately excluded. Loading from bytes avoids an
    // unaligned uint32_t access through the packed on-disk structure.
    const auto bytes = std::as_bytes(std::span{&header, 1});
    std::uint32_t sum = 0;
    for (std::size_t offset = sizeof(std::uint32_t); offset < bytes.size(); offset += sizeof(std::uint32_t)) {
        sum += load_u32_le(bytes.data() + offset);
    }
    return sum;
}

std::string partition_id(const Header& header)
{
    const auto end = std::find(std::begin(header.id), std::end(header.id), '\0');
    return std::string(std::begin(header.id), end);
}

bool has_sony_mbr_magic(const Header& header) noexcept
{
    constexpr std::size_t magic_len = sizeof(kSonyMbrMagic) - 1;
    static_assert(magic_len <= sizeof(header.mbr.magic));
    return std::memcmp(header.mbr.magic, kSonyMbrMagic, magic_len) == 0;
}

std::string type_name(std::uint16_t type)
{
    switch (type) {
    case kTypeFree: return "FREE";
    case kTypeMbr: return "MBR";
    case 0x0082: return "EXT2SWAP";
    case 0x0083: return "EXT2";
    case 0x0088: return "REISER";
    case kTypePfs: return "PFS";
    case 0x0101: return "CFS";
    case kTypeHdl: return "HDL";
    default: return "UNKNOWN";
    }
}

bool Reader::read_header(std::uint32_t lba, Header& out)
{
    const std::uint64_t offset = static_cast<std::uint64_t>(lba) * kSectorSize;
    if (!range_fits(offset, sizeof(out), device_.size_bytes())) {
        return false;
    }
    return device_.read(offset, std::as_writable_bytes(std::span{&out, 1}));
}

ScanResult Reader::scan(std::size_t max_headers)
{
    ScanResult result;

    if (device_.size_bytes() < kHeaderSize) {
        result.issues.push_back({IssueSeverity::error, 0, "Device is smaller than one APA header"});
        return result;
    }

    Header header{};
    if (!read_header(0, header)) {
        result.issues.push_back({IssueSeverity::error, 0, "Could not read APA MBR header"});
        return result;
    }
    if (header.magic != kMagic) {
        result.issues.push_back({IssueSeverity::error, 0, "APA magic is missing at LBA 0"});
        return result;
    }
    if (checksum(header) != header.checksum) {
        result.issues.push_back({IssueSeverity::error, 0, "APA MBR checksum mismatch"});
        return result;
    }
    if (!has_sony_mbr_magic(header)) {
        result.issues.push_back({IssueSeverity::error, 0, "Sony APA MBR signature is missing"});
        return result;
    }

    result.mbr_valid = true;
    result.apa_version = header.mbr.version;

    // APA is a linked list of headers, not a flat table at fixed offsets. Keep
    // both explicit visited-LBA state and a hard header limit: corrupted `next`
    // links must never be able to make a raw-disk scan loop forever.
    std::unordered_set<std::uint32_t> visited;
    std::uint32_t current_lba = 0;
    std::uint32_t expected_prev = header.prev;

    for (std::size_t count = 0; count < max_headers; ++count) {
        if (!visited.insert(current_lba).second) {
            result.issues.push_back({IssueSeverity::error, current_lba, "Cycle detected in APA partition chain"});
            break;
        }

        if (count != 0) {
            if (!read_header(current_lba, header)) {
                result.issues.push_back({IssueSeverity::error, current_lba, "Could not read APA header"});
                break;
            }
            if (header.magic != kMagic) {
                result.issues.push_back({IssueSeverity::error, current_lba, "Invalid APA magic"});
                break;
            }
            if (checksum(header) != header.checksum) {
                result.issues.push_back({IssueSeverity::error, current_lba, "APA checksum mismatch"});
                break;
            }
        }

        if (header.start != current_lba) {
            result.issues.push_back({IssueSeverity::warning, current_lba, "Header start LBA does not match its physical location"});
        }
        if (count != 0 && header.prev != expected_prev) {
            result.issues.push_back({IssueSeverity::warning, current_lba, "APA prev link does not point to the previous header"});
        }
        if (header.nsub > kMaxSub) {
            result.issues.push_back({IssueSeverity::warning, current_lba, "APA sub-partition count exceeds format limit; clamping to 64"});
        }

        // Header links being in range is not enough: damaged metadata can point
        // a partition extent beyond the end of the device while leaving the
        // header itself readable. Reject that before any filesystem layer sees
        // the partition as a usable address space.
        if (!sector_extent_fits(header.start, header.length, device_.size_bytes())) {
            result.issues.push_back({IssueSeverity::error, current_lba, "APA partition extent extends outside the device"});
        }

        Partition partition;
        partition.id = partition_id(header);
        partition.start_lba = header.start;
        partition.length_sectors = header.length;
        partition.type = header.type;
        partition.flags = header.flags;
        partition.main_lba = header.main;
        partition.number = header.number;
        partition.sub_count = std::min<std::uint32_t>(header.nsub, static_cast<std::uint32_t>(kMaxSub));
        partition.created = header.created;
        partition.total_sectors = header.length;
        partition.sub_partitions.reserve(partition.sub_count);

        // Keep the actual sub extents instead of collapsing them into one size.
        // Logical size is useful for UI, but PFS must later translate each
        // BlockInfo.subpart through the real start LBA because physical extents
        // are not guaranteed to be adjacent.
        for (std::uint32_t i = 0; i < partition.sub_count; ++i) {
            const auto& sub = header.subs[i];
            if (!sector_extent_fits(sub.start, sub.length, device_.size_bytes())) {
                result.issues.push_back({IssueSeverity::error, current_lba,
                                         "APA sub-partition extent extends outside the device"});
            }
            partition.sub_partitions.push_back(sub);
            partition.total_sectors += sub.length;
        }
        result.partitions.push_back(std::move(partition));

        if (header.next == 0) {
            break;
        }

        const std::uint64_t next_offset = static_cast<std::uint64_t>(header.next) * kSectorSize;
        if (!range_fits(next_offset, kHeaderSize, device_.size_bytes())) {
            result.issues.push_back({IssueSeverity::error, header.next, "APA next link points outside the device"});
            break;
        }

        expected_prev = current_lba;
        current_lba = header.next;

        if (count + 1 == max_headers) {
            result.issues.push_back({IssueSeverity::error, current_lba, "APA scan header limit reached"});
        }
    }

    return result;
}

} // namespace ps2hdd::apa
