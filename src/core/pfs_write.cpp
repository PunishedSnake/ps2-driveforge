#include "ps2hdd/pfs_write.hpp"

#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace ps2hdd::pfs {

static_assert(std::endian::native == std::endian::little,
              "PFS writer currently requires a little-endian host");

namespace {

constexpr std::uint32_t kBitmapBitsPerChunk = 8192;
constexpr std::uint32_t kMetadataSectors =
    static_cast<std::uint32_t>(kMetadataSize / apa::kSectorSize);
constexpr std::uint32_t kMainBitmapOffsetMetadataBlocks = 0x1000;

std::size_t align4(std::size_t value) noexcept
{
    return (value + 3U) & ~std::size_t{3U};
}

std::string normalize_path(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    bool previous_slash = false;
    for (const char ch : input) {
        const char normalized = ch == '\\' ? '/' : ch;
        if (normalized == '/') {
            if (!previous_slash) {
                out.push_back('/');
            }
            previous_slash = true;
        } else {
            out.push_back(normalized);
            previous_slash = false;
        }
    }
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

bool contains_parent_escape(std::string_view path)
{
    std::size_t cursor = 0;
    while (cursor < path.size()) {
        while (cursor < path.size() && path[cursor] == '/') {
            ++cursor;
        }
        const auto begin = cursor;
        while (cursor < path.size() && path[cursor] != '/') {
            ++cursor;
        }
        if (path.substr(begin, cursor - begin) == "..") {
            return true;
        }
    }
    return false;
}

void store_u16(std::byte* target, std::uint16_t value) noexcept
{
    std::memcpy(target, &value, sizeof(value));
}

void store_u32(std::byte* target, std::uint32_t value) noexcept
{
    std::memcpy(target, &value, sizeof(value));
}

std::uint16_t load_u16(const std::byte* source) noexcept
{
    std::uint16_t value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

std::uint32_t load_u32(const std::byte* source) noexcept
{
    std::uint32_t value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

bool append_unique(std::vector<ImageWriter::BitmapKey>& dirty,
                   const ImageWriter::BitmapKey& key)
{
    if (std::find(dirty.begin(), dirty.end(), key) != dirty.end()) {
        return false;
    }
    dirty.push_back(key);
    return true;
}

} // namespace

ImageWriter::ImageWriter(WritableBlockDevice& device, const apa::Partition& partition)
    : device_(device),
      partition_(partition),
      writable_volume_(device, partition),
      read_volume_(device, partition),
      probe_(probe(read_volume_))
{
    if (partition.type != apa::kTypePfs || partition.is_sub()) {
        probe_.valid = false;
        error_ = "PFS writer requires a main APA PFS partition";
    } else if (!probe_.valid) {
        error_ = probe_.errors.empty() ? "PFS probe failed" : probe_.errors.front();
    }
}

unsigned ImageWriter::inode_scale() const noexcept
{
    unsigned scale = 0;
    std::uint32_t size = static_cast<std::uint32_t>(kMetadataSize);
    while (size < probe_.super.zone_size) {
        size <<= 1U;
        ++scale;
    }
    return scale;
}

std::uint32_t ImageWriter::sectors_per_zone() const noexcept
{
    return probe_.super.zone_size / apa::kSectorSize;
}

std::uint64_t ImageWriter::zones_in_subpart(std::size_t subpart) const noexcept
{
    if (subpart >= writable_volume_.extent_count() || sectors_per_zone() == 0) {
        return 0;
    }
    return writable_volume_.extent(subpart).length_sectors / sectors_per_zone();
}

std::uint32_t ImageWriter::bitmap_sector(std::size_t subpart,
                                         std::uint32_t chunk) const noexcept
{
    const std::uint64_t metadata_block =
        static_cast<std::uint64_t>(1U << inode_scale()) + chunk +
        (subpart == 0 ? kMainBitmapOffsetMetadataBlocks : 0U);
    const std::uint64_t sector = metadata_block * kMetadataSectors;
    if (sector > std::numeric_limits<std::uint32_t>::max()) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(sector);
}

ImageWriter::BitmapBytes* ImageWriter::bitmap(std::size_t subpart, std::uint32_t chunk)
{
    const BitmapKey key{subpart, chunk};
    if (const auto found = bitmap_cache_.find(key); found != bitmap_cache_.end()) {
        return &found->second;
    }

    const auto sector = bitmap_sector(subpart, chunk);
    if (sector == std::numeric_limits<std::uint32_t>::max()) {
        error_ = "PFS bitmap sector address overflow";
        return nullptr;
    }

    BitmapBytes bytes{};
    if (!writable_volume_.read_sectors(subpart, sector, kMetadataSectors, bytes)) {
        error_ = "Could not read PFS bitmap metadata";
        return nullptr;
    }
    return &bitmap_cache_.emplace(key, bytes).first->second;
}

bool ImageWriter::zone_used(std::size_t subpart, std::uint32_t zone)
{
    if (zone >= zones_in_subpart(subpart)) {
        return true;
    }
    const auto chunk = zone / kBitmapBitsPerChunk;
    const auto bit = zone % kBitmapBitsPerChunk;
    auto* bytes = bitmap(subpart, chunk);
    if (bytes == nullptr) {
        return true;
    }
    const auto byte_index = bit / 8U;
    const auto bit_index = bit % 8U;
    const auto value = std::to_integer<unsigned char>((*bytes)[byte_index]);
    return (value & (1U << bit_index)) != 0;
}

bool ImageWriter::set_zone(std::size_t subpart, std::uint32_t zone, bool used,
                           std::vector<BitmapKey>& dirty, std::string& error)
{
    if (zone >= zones_in_subpart(subpart)) {
        error = "PFS bitmap mutation targets a zone outside the APA extent";
        return false;
    }
    const auto chunk = zone / kBitmapBitsPerChunk;
    const auto bit = zone % kBitmapBitsPerChunk;
    auto* bytes = bitmap(subpart, chunk);
    if (bytes == nullptr) {
        error = error_.empty() ? "Could not load PFS bitmap" : error_;
        return false;
    }
    const auto byte_index = bit / 8U;
    const auto bit_index = bit % 8U;
    const auto mask = static_cast<unsigned char>(1U << bit_index);
    auto value = std::to_integer<unsigned char>((*bytes)[byte_index]);
    const bool current = (value & mask) != 0;
    if (current == used) {
        error = used ? "PFS allocator tried to reserve an already-used zone"
                     : "PFS allocator tried to release an already-free zone";
        return false;
    }
    value = used ? static_cast<unsigned char>(value | mask)
                 : static_cast<unsigned char>(value & static_cast<unsigned char>(~mask));
    (*bytes)[byte_index] = static_cast<std::byte>(value);
    append_unique(dirty, {subpart, chunk});
    return true;
}

ImageWriter::ZoneRun ImageWriter::find_free_run(std::uint32_t count,
                                                std::size_t preferred_subpart,
                                                std::uint32_t preferred_zone)
{
    if (count == 0) {
        return {preferred_subpart, preferred_zone, 0};
    }

    std::vector<std::size_t> order;
    if (preferred_subpart < writable_volume_.extent_count()) {
        order.push_back(preferred_subpart);
    }
    for (std::size_t sub = 0; sub < writable_volume_.extent_count(); ++sub) {
        if (sub != preferred_subpart) {
            order.push_back(sub);
        }
    }

    for (const auto subpart : order) {
        const auto total64 = zones_in_subpart(subpart);
        if (total64 < count || total64 > std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }
        const auto total = static_cast<std::uint32_t>(total64);
        const auto start = subpart == preferred_subpart && preferred_zone < total
                               ? preferred_zone
                               : 0U;

        const auto scan = [&](std::uint32_t begin, std::uint32_t end) -> ZoneRun {
            std::uint32_t run_start = 0;
            std::uint32_t run_count = 0;
            for (std::uint32_t zone = begin; zone < end; ++zone) {
                if (!zone_used(subpart, zone)) {
                    if (run_count == 0) {
                        run_start = zone;
                    }
                    ++run_count;
                    if (run_count == count) {
                        return {subpart, run_start, count};
                    }
                } else {
                    run_count = 0;
                }
            }
            return {};
        };

        if (const auto run = scan(start, total); run.count != 0) {
            return run;
        }
        if (start != 0) {
            if (const auto run = scan(0, start); run.count != 0) {
                return run;
            }
        }
        if (!error_.empty()) {
            return {};
        }
    }
    return {};
}

void ImageWriter::discard_bitmap_cache(const std::vector<BitmapKey>& dirty)
{
    for (const auto& key : dirty) {
        bitmap_cache_.erase(key);
    }
}

bool ImageWriter::commit_bitmap_changes(const std::vector<BitmapKey>& dirty,
                                        FileWriteResult& result,
                                        std::string_view label)
{
    if (dirty.empty()) {
        return true;
    }

    WriteTransaction transaction(device_);
    for (const auto& key : dirty) {
        const auto found = bitmap_cache_.find(key);
        if (found == bitmap_cache_.end()) {
            result.error = "PFS bitmap cache lost a dirty chunk before commit";
            discard_bitmap_cache(dirty);
            return false;
        }
        const auto sector = bitmap_sector(key.first, key.second);
        std::uint64_t offset = 0;
        if (!writable_volume_.absolute_byte_offset(key.first, sector, kMetadataSectors, offset) ||
            !transaction.stage(offset, found->second, label)) {
            result.error = "Could not stage PFS bitmap transaction: " + transaction.stage_error();
            discard_bitmap_cache(dirty);
            return false;
        }
    }

    const auto committed = transaction.commit();
    if (!committed.ok) {
        result.error = "PFS bitmap transaction failed: " + committed.error;
        discard_bitmap_cache(dirty);
        return false;
    }
    ++result.metadata_transactions;
    result.bitmap_chunks_touched += dirty.size();
    return true;
}

bool ImageWriter::write_payload(const ZoneRun& run, std::span<const std::byte> bytes,
                                std::size_t batch_bytes, FileWriteResult& result)
{
    if (bytes.empty()) {
        return true;
    }
    const std::uint64_t capacity =
        static_cast<std::uint64_t>(run.count) * probe_.super.zone_size;
    if (bytes.size() > capacity) {
        result.error = "PFS payload exceeds reserved zone run";
        return false;
    }

    std::size_t batch = std::max<std::size_t>(apa::kSectorSize, batch_bytes);
    batch -= batch % apa::kSectorSize;
    const auto first_sector64 = static_cast<std::uint64_t>(run.first) * sectors_per_zone();
    if (first_sector64 > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "PFS payload sector address overflow";
        return false;
    }

    std::size_t position = 0;
    std::uint32_t sector = static_cast<std::uint32_t>(first_sector64);
    while (position + apa::kSectorSize <= bytes.size()) {
        const auto remaining_full = (bytes.size() - position) / apa::kSectorSize * apa::kSectorSize;
        const auto take = std::min(batch, remaining_full);
        const auto sectors = static_cast<std::uint32_t>(take / apa::kSectorSize);
        if (!writable_volume_.write_sectors(run.subpart, sector, sectors,
                                            bytes.subspan(position, take))) {
            result.error = "PFS payload write failed";
            return false;
        }
        ++result.payload_write_calls;
        position += take;
        sector += sectors;
    }

    if (position < bytes.size()) {
        std::array<std::byte, apa::kSectorSize> tail{};
        std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(position), bytes.end(), tail.begin());
        if (!writable_volume_.write_sectors(run.subpart, sector, 1, tail)) {
            result.error = "PFS payload tail write failed";
            return false;
        }
        ++result.payload_write_calls;
    }
    if (!writable_volume_.flush()) {
        result.error = "PFS payload flush failed";
        return false;
    }
    return true;
}

bool ImageWriter::verify_payload(const ZoneRun& run, std::span<const std::byte> bytes,
                                 std::size_t batch_bytes)
{
    if (bytes.empty()) {
        return true;
    }
    std::size_t batch = std::max<std::size_t>(apa::kSectorSize, batch_bytes);
    batch -= batch % apa::kSectorSize;
    std::vector<std::byte> buffer(batch);

    const auto first_sector64 = static_cast<std::uint64_t>(run.first) * sectors_per_zone();
    if (first_sector64 > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::uint32_t sector = static_cast<std::uint32_t>(first_sector64);
    std::size_t position = 0;
    while (position < bytes.size()) {
        const auto remaining = bytes.size() - position;
        const auto payload_take = std::min(batch, remaining);
        const auto sectors = static_cast<std::uint32_t>((payload_take + apa::kSectorSize - 1) /
                                                        apa::kSectorSize);
        const auto disk_take = static_cast<std::size_t>(sectors) * apa::kSectorSize;
        if (buffer.size() < disk_take) {
            buffer.resize(disk_take);
        }
        if (!writable_volume_.read_sectors(run.subpart, sector, sectors,
                                           std::span<std::byte>(buffer.data(), disk_take))) {
            return false;
        }
        if (!std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                        bytes.begin() + static_cast<std::ptrdiff_t>(position + payload_take),
                        buffer.begin())) {
            return false;
        }
        position += payload_take;
        sector += sectors;
    }
    return true;
}

bool ImageWriter::stage_inode(const Inode& inode, std::string_view label,
                              FileWriteResult& result)
{
    const auto metadata_block64 = static_cast<std::uint64_t>(inode.inode_block.number) << inode_scale();
    const auto sector64 = metadata_block64 * kMetadataSectors;
    if (sector64 > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "PFS inode sector address overflow";
        return false;
    }
    std::uint64_t offset = 0;
    if (!writable_volume_.absolute_byte_offset(inode.inode_block.subpart,
                                               static_cast<std::uint32_t>(sector64),
                                               kMetadataSectors, offset)) {
        result.error = "PFS inode maps outside its APA extent";
        return false;
    }

    WriteTransaction transaction(device_);
    if (!transaction.stage(offset, std::as_bytes(std::span{&inode, 1}), label)) {
        result.error = "Could not stage PFS inode: " + transaction.stage_error();
        return false;
    }
    const auto committed = transaction.commit();
    if (!committed.ok) {
        result.error = "PFS inode transaction failed: " + committed.error;
        return false;
    }
    ++result.metadata_transactions;
    return true;
}

ImageWriter::DirectorySlot ImageWriter::plan_dentry_insert(const Node& parent,
                                                           std::string_view name,
                                                           const BlockInfo& inode_location)
{
    DirectorySlot slot;
    if ((parent.inode.mode & kModeMask) != kModeDirectory) {
        slot.error = "PFS parent is not a directory";
        return slot;
    }
    if (name.empty() || name.size() > 255) {
        slot.error = "PFS filename must contain 1..255 bytes";
        return slot;
    }
    if (parent.inode.number_data <= 1 || parent.inode.number_data > kInodeMaxBlocks ||
        parent.inode.number_segdesg != 1 || parent.inode.next_segment.number != 0) {
        slot.error = "PFS directory uses an indirect/unsupported data layout";
        return slot;
    }
    if (parent.inode.size == 0 || parent.inode.size > 64ULL * 1024ULL * 1024ULL) {
        slot.error = "PFS directory size is invalid for bounded writer";
        return slot;
    }

    Reader reader(read_volume_, probe_);
    std::vector<std::byte> directory(static_cast<std::size_t>(parent.inode.size));
    if (!reader.read(parent, 0, directory)) {
        slot.error = "Could not read parent PFS directory: " + reader.last_error();
        return slot;
    }

    const std::size_t requested = align4(8U + name.size());
    std::size_t chosen_position = std::numeric_limits<std::size_t>::max();
    std::size_t existing_actual = 0;
    std::uint16_t existing_mode = 0;
    bool reuse_empty = false;

    std::size_t position = 0;
    while (position < directory.size()) {
        const auto sector_end = std::min(((position / apa::kSectorSize) + 1U) * apa::kSectorSize,
                                         directory.size());
        if (position + 8 > sector_end) {
            position = sector_end;
            continue;
        }
        const auto* ptr = directory.data() + static_cast<std::ptrdiff_t>(position);
        const auto inode = load_u32(ptr);
        const auto path_len = std::to_integer<unsigned char>(ptr[5]);
        const auto raw_len = load_u16(ptr + 6);
        const auto allocated = static_cast<std::size_t>(raw_len & 0x0FFFU);
        const auto mode = static_cast<std::uint16_t>(raw_len & kModeMask);
        if (allocated < 8 || (allocated & 3U) != 0 || position + allocated > sector_end) {
            slot.error = "Malformed PFS directory entry while planning insertion";
            return slot;
        }
        const auto actual = align4(8U + path_len);
        if (actual > allocated) {
            slot.error = "Malformed PFS directory entry has no room for its own name";
            return slot;
        }

        if ((inode == 0 || path_len == 0) && allocated >= requested) {
            chosen_position = position;
            existing_actual = 0;
            existing_mode = mode;
            reuse_empty = true;
            break;
        }
        if (allocated >= actual + requested) {
            chosen_position = position;
            existing_actual = actual;
            existing_mode = mode;
            break;
        }
        position += allocated;
    }

    if (chosen_position == std::numeric_limits<std::size_t>::max()) {
        slot.error = "PFS directory has no reusable dentry space; directory growth is not enabled yet";
        return slot;
    }

    const std::size_t logical_sector_start =
        (chosen_position / apa::kSectorSize) * apa::kSectorSize;
    std::uint64_t logical = 0;
    bool mapped = false;
    for (std::size_t index = 1; index < parent.inode.number_data; ++index) {
        const auto& block = parent.inode.data[index];
        const std::uint64_t extent_bytes =
            static_cast<std::uint64_t>(block.count) * probe_.super.zone_size;
        if (logical_sector_start >= logical && logical_sector_start < logical + extent_bytes) {
            const auto within = logical_sector_start - logical;
            if ((within % apa::kSectorSize) != 0) {
                slot.error = "PFS directory sector mapping is not sector aligned";
                return slot;
            }
            const auto sector64 = static_cast<std::uint64_t>(block.number) * sectors_per_zone() +
                                  within / apa::kSectorSize;
            if (sector64 > std::numeric_limits<std::uint32_t>::max()) {
                slot.error = "PFS directory sector address overflow";
                return slot;
            }
            slot.subpart = block.subpart;
            slot.sector = static_cast<std::uint32_t>(sector64);
            mapped = true;
            break;
        }
        logical += extent_bytes;
    }
    if (!mapped || !writable_volume_.read_sectors(slot.subpart, slot.sector, 1, slot.sector_bytes)) {
        slot.error = "Could not map/read PFS directory sector for insertion";
        return slot;
    }

    const auto in_sector = chosen_position % apa::kSectorSize;
    auto* ptr = slot.sector_bytes.data() + static_cast<std::ptrdiff_t>(in_sector);
    const auto old_raw = load_u16(ptr + 6);
    const auto old_allocated = static_cast<std::size_t>(old_raw & 0x0FFFU);
    std::size_t new_position = in_sector;
    std::size_t new_allocated = old_allocated;

    if (!reuse_empty) {
        const auto shrunk = static_cast<std::uint16_t>(existing_mode | existing_actual);
        store_u16(ptr + 6, shrunk);
        new_position += existing_actual;
        new_allocated -= existing_actual;
    }

    auto* target = slot.sector_bytes.data() + static_cast<std::ptrdiff_t>(new_position);
    std::fill(target, target + static_cast<std::ptrdiff_t>(new_allocated), std::byte{0});
    store_u32(target, inode_location.number);
    target[4] = static_cast<std::byte>(inode_location.subpart);
    target[5] = static_cast<std::byte>(name.size());
    store_u16(target + 6, static_cast<std::uint16_t>(kModeRegular | new_allocated));
    std::memcpy(target + 8, name.data(), name.size());
    slot.ok = true;
    return slot;
}

bool ImageWriter::publish_dentry(const DirectorySlot& slot, FileWriteResult& result)
{
    std::uint64_t offset = 0;
    if (!writable_volume_.absolute_byte_offset(slot.subpart, slot.sector, 1, offset)) {
        result.error = "PFS dentry sector maps outside its APA extent";
        return false;
    }
    WriteTransaction transaction(device_);
    if (!transaction.stage(offset, slot.sector_bytes, "publish PFS directory entry")) {
        result.error = "Could not stage PFS dentry: " + transaction.stage_error();
        return false;
    }
    const auto committed = transaction.commit();
    if (!committed.ok) {
        result.error = "PFS dentry transaction failed: " + committed.error;
        return false;
    }
    ++result.metadata_transactions;
    return true;
}

bool ImageWriter::verify_file(std::string_view path, std::span<const std::byte> expected)
{
    Reader reader(read_volume_);
    if (!reader.valid()) {
        return false;
    }
    const auto node = reader.resolve(path);
    if (!node || node->inode.size != expected.size()) {
        return false;
    }
    std::vector<std::byte> actual(expected.size());
    if (!actual.empty() && !reader.read(*node, 0, actual)) {
        return false;
    }
    return std::equal(expected.begin(), expected.end(), actual.begin(), actual.end());
}

FileWriteResult ImageWriter::create_file(const Node& parent, std::string_view full_path,
                                         std::string_view name, std::span<const std::byte> bytes,
                                         const FileWriteOptions& options)
{
    FileWriteResult result;
    result.disposition = FileWriteDisposition::created;
    result.path = std::string(full_path);
    result.bytes = bytes.size();

    const auto zone_size = static_cast<std::uint64_t>(probe_.super.zone_size);
    const auto data_zones64 = (bytes.size() + zone_size - 1U) / zone_size;
    if (data_zones64 > std::numeric_limits<std::uint16_t>::max()) {
        result.error = "PFS small-file writer would require more than 65535 data zones";
        return result;
    }
    const auto data_zones = static_cast<std::uint32_t>(data_zones64);
    const auto total_zones = 1U + data_zones;
    const auto preferred_sub = static_cast<std::size_t>(parent.inode.inode_block.subpart);
    const auto run = find_free_run(total_zones, preferred_sub, parent.inode.inode_block.number);
    if (run.count != total_zones) {
        result.error = error_.empty() ? "No contiguous PFS zone run is available for the new file"
                                      : error_;
        return result;
    }

    const BlockInfo inode_location{run.first, static_cast<std::uint16_t>(run.subpart), 1};
    const auto dentry = plan_dentry_insert(parent, name, inode_location);
    if (!dentry.ok) {
        result.error = dentry.error;
        return result;
    }

    std::vector<BitmapKey> reserved_dirty;
    std::string bitmap_error;
    for (std::uint32_t i = 0; i < total_zones; ++i) {
        if (!set_zone(run.subpart, run.first + i, true, reserved_dirty, bitmap_error)) {
            result.error = bitmap_error;
            discard_bitmap_cache(reserved_dirty);
            return result;
        }
    }
    if (!commit_bitmap_changes(reserved_dirty, result, "reserve PFS zones for new file")) {
        return result;
    }

    const auto cleanup_reserved = [&]() {
        std::vector<BitmapKey> dirty;
        std::string cleanup_error;
        for (std::uint32_t i = 0; i < total_zones; ++i) {
            if (!set_zone(run.subpart, run.first + i, false, dirty, cleanup_error)) {
                result.warning = "Could not release reserved PFS zones after failed create: " + cleanup_error;
                return;
            }
        }
        const auto saved_error = result.error;
        if (!commit_bitmap_changes(dirty, result, "release abandoned PFS create zones")) {
            result.warning = "PFS create failed safely, but reserved zones could not be released: " + result.error;
        }
        result.error = saved_error;
    };

    const ZoneRun payload_run{run.subpart, run.first + 1U, data_zones};
    if (!write_payload(payload_run, bytes, options.io_batch_bytes, result) ||
        !verify_payload(payload_run, bytes, options.io_batch_bytes)) {
        if (result.error.empty()) {
            result.error = "PFS payload readback verification failed";
        }
        cleanup_reserved();
        return result;
    }

    Inode inode{};
    inode.magic = kSegdMagic;
    inode.inode_block = inode_location;
    inode.last_segment = inode_location;
    inode.data[0] = inode_location;
    if (data_zones != 0) {
        inode.data[1] = {run.first + 1U, static_cast<std::uint16_t>(run.subpart),
                         static_cast<std::uint16_t>(data_zones)};
    }
    inode.mode = options.mode;
    inode.uid = options.uid;
    inode.gid = options.gid;
    inode.atime = options.timestamp;
    inode.ctime = options.timestamp;
    inode.mtime = options.timestamp;
    inode.size = bytes.size();
    inode.number_blocks = 1U + data_zones;
    inode.number_data = data_zones == 0 ? 1U : 2U;
    inode.number_segdesg = 1;
    inode.subpart = static_cast<std::uint32_t>(run.subpart);
    inode.checksum = inode_checksum(inode);

    if (!stage_inode(inode, "publish new PFS inode", result)) {
        cleanup_reserved();
        return result;
    }
    if (!publish_dentry(dentry, result)) {
        cleanup_reserved();
        return result;
    }
    if (!verify_file(full_path, bytes)) {
        result.error = "PFS create committed but cold reader verification failed";
        return result;
    }

    result.ok = true;
    return result;
}

FileWriteResult ImageWriter::replace_file(const Node& node, std::string_view full_path,
                                          std::span<const std::byte> bytes,
                                          const FileWriteOptions& options)
{
    FileWriteResult result;
    result.disposition = FileWriteDisposition::replaced;
    result.path = std::string(full_path);
    result.bytes = bytes.size();

    if ((node.inode.mode & kModeMask) != kModeRegular) {
        result.error = "PFS replacement target is not a regular file";
        return result;
    }
    if (node.inode.number_data == 0 || node.inode.number_data > kInodeMaxBlocks ||
        node.inode.number_segdesg != 1 || node.inode.next_segment.number != 0) {
        result.error = "PFS replacement target uses an indirect/unsupported data layout";
        return result;
    }

    std::vector<ZoneRun> old_runs;
    for (std::size_t i = 1; i < node.inode.number_data; ++i) {
        const auto& block = node.inode.data[i];
        if (block.count == 0 || block.subpart >= writable_volume_.extent_count()) {
            result.error = "PFS replacement target contains an invalid data extent";
            return result;
        }
        for (std::uint32_t zone = 0; zone < block.count; ++zone) {
            if (!zone_used(block.subpart, block.number + zone)) {
                result.error = "PFS replacement target references a bitmap-free data zone";
                return result;
            }
        }
        old_runs.push_back({block.subpart, block.number, block.count});
    }

    const auto zone_size = static_cast<std::uint64_t>(probe_.super.zone_size);
    const auto data_zones64 = (bytes.size() + zone_size - 1U) / zone_size;
    if (data_zones64 > std::numeric_limits<std::uint16_t>::max()) {
        result.error = "PFS replacement would require more than 65535 data zones";
        return result;
    }
    const auto data_zones = static_cast<std::uint32_t>(data_zones64);

    ZoneRun new_run{};
    std::vector<BitmapKey> reserve_dirty;
    if (data_zones != 0) {
        const std::size_t preferred_sub = old_runs.empty()
                                              ? node.inode.inode_block.subpart
                                              : old_runs.front().subpart;
        const auto preferred_zone = old_runs.empty()
                                        ? node.inode.inode_block.number
                                        : old_runs.front().first;
        new_run = find_free_run(data_zones, preferred_sub, preferred_zone);
        if (new_run.count != data_zones) {
            result.error = error_.empty() ? "No contiguous PFS run is available for copy-on-write replacement"
                                          : error_;
            return result;
        }
        std::string bitmap_error;
        for (std::uint32_t i = 0; i < data_zones; ++i) {
            if (!set_zone(new_run.subpart, new_run.first + i, true, reserve_dirty, bitmap_error)) {
                result.error = bitmap_error;
                discard_bitmap_cache(reserve_dirty);
                return result;
            }
        }
        if (!commit_bitmap_changes(reserve_dirty, result, "reserve PFS copy-on-write data zones")) {
            return result;
        }
        if (!write_payload(new_run, bytes, options.io_batch_bytes, result) ||
            !verify_payload(new_run, bytes, options.io_batch_bytes)) {
            if (result.error.empty()) {
                result.error = "PFS copy-on-write payload verification failed";
            }
            std::vector<BitmapKey> release;
            std::string release_error;
            for (std::uint32_t i = 0; i < data_zones; ++i) {
                if (!set_zone(new_run.subpart, new_run.first + i, false, release, release_error)) {
                    result.warning = "Replacement failed before publication and reserved zones could not be released: " + release_error;
                    return result;
                }
            }
            const auto saved_error = result.error;
            if (!commit_bitmap_changes(release, result, "release failed replacement zones")) {
                result.warning = "Replacement failed safely, but reserved zones remain allocated: " + result.error;
            }
            result.error = saved_error;
            return result;
        }
    }

    Inode replacement = node.inode;
    replacement.next_segment = {};
    replacement.last_segment = replacement.inode_block;
    for (std::size_t i = 1; i < kInodeMaxBlocks; ++i) {
        replacement.data[i] = {};
    }
    if (data_zones != 0) {
        replacement.data[1] = {new_run.first, static_cast<std::uint16_t>(new_run.subpart),
                               static_cast<std::uint16_t>(data_zones)};
    }
    replacement.size = bytes.size();
    replacement.number_blocks = 1U + data_zones;
    replacement.number_data = data_zones == 0 ? 1U : 2U;
    replacement.number_segdesg = 1;
    replacement.atime = options.timestamp;
    replacement.mtime = options.timestamp;
    replacement.checksum = inode_checksum(replacement);

    const auto metadata_block64 = static_cast<std::uint64_t>(replacement.inode_block.number) << inode_scale();
    const auto sector64 = metadata_block64 * kMetadataSectors;
    std::uint64_t inode_offset = 0;
    if (sector64 > std::numeric_limits<std::uint32_t>::max() ||
        !writable_volume_.absolute_byte_offset(replacement.inode_block.subpart,
                                               static_cast<std::uint32_t>(sector64),
                                               kMetadataSectors, inode_offset)) {
        result.error = "Replacement inode address is outside the PFS APA extent";
        return result;
    }

    WriteTransaction transaction(device_);
    if (!transaction.stage(inode_offset, std::as_bytes(std::span{&replacement, 1}),
                           "switch PFS inode to copy-on-write payload")) {
        result.error = "Could not stage replacement inode: " + transaction.stage_error();
        return result;
    }
    const auto committed = transaction.commit([&]() -> std::string {
        return verify_file(full_path, bytes) ? std::string{}
                                             : std::string{"PFS cold reader did not verify replacement"};
    });
    if (!committed.ok) {
        result.error = "PFS replacement inode transaction failed: " + committed.error;
        if (data_zones != 0) {
            std::vector<BitmapKey> release;
            std::string release_error;
            for (std::uint32_t i = 0; i < data_zones; ++i) {
                if (!set_zone(new_run.subpart, new_run.first + i, false, release, release_error)) {
                    result.warning = "Replacement rolled back, but new zones could not be released: " + release_error;
                    return result;
                }
            }
            const auto saved_error = result.error;
            if (!commit_bitmap_changes(release, result, "release rolled-back replacement zones")) {
                result.warning = "Replacement rolled back, but new zones remain allocated: " + result.error;
            }
            result.error = saved_error;
        }
        return result;
    }
    ++result.metadata_transactions;

    std::vector<BitmapKey> release_old;
    std::string release_error;
    for (const auto& old : old_runs) {
        for (std::uint32_t i = 0; i < old.count; ++i) {
            if (!set_zone(old.subpart, old.first + i, false, release_old, release_error)) {
                result.warning = "File replacement is valid, but old PFS zones could not be released: " + release_error;
                result.ok = true;
                return result;
            }
        }
    }
    if (!release_old.empty()) {
        const auto saved_error = result.error;
        if (!commit_bitmap_changes(release_old, result, "release old copy-on-write PFS zones")) {
            result.warning = "File replacement is valid, but old PFS zones remain allocated: " + result.error;
            result.error = saved_error;
            result.ok = true;
            return result;
        }
    }

    result.ok = true;
    return result;
}

FileWriteResult ImageWriter::write_file(std::string_view input_path,
                                        std::span<const std::byte> bytes,
                                        const FileWriteOptions& options)
{
    FileWriteResult result;
    if (!valid()) {
        result.error = error_.empty() ? "PFS writer session is invalid" : error_;
        return result;
    }
    if (options.io_batch_bytes < apa::kSectorSize) {
        result.error = "PFS I/O batch must be at least one disk sector";
        return result;
    }

    const std::string path = normalize_path(input_path);
    if (path.empty() || path == "/" || contains_parent_escape(path)) {
        result.error = "PFS writer requires a normal file path without '..' traversal";
        return result;
    }
    const auto slash = path.find_last_of('/');
    const std::string parent_path = slash == std::string::npos || slash == 0
                                        ? "/"
                                        : path.substr(0, slash);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.empty() || name == "." || name == ".." || name.size() > 255) {
        result.error = "PFS filename must contain 1..255 non-special bytes";
        return result;
    }

    Reader reader(read_volume_);
    if (!reader.valid()) {
        result.error = "PFS changed or became invalid before write session operation";
        return result;
    }
    const auto parent = reader.resolve(parent_path);
    if (!parent) {
        result.error = "PFS parent directory does not exist: " + reader.last_error();
        return result;
    }
    if ((parent->inode.mode & kModeMask) != kModeDirectory) {
        result.error = "PFS parent path is not a directory";
        return result;
    }

    const auto entries = reader.list_directory(*parent, true);
    if (!reader.last_error().empty()) {
        result.error = "Could not enumerate PFS parent directory: " + reader.last_error();
        return result;
    }
    const auto existing = std::find_if(entries.begin(), entries.end(), [&](const DirectoryEntry& entry) {
        return entry.name == name;
    });
    if (existing != entries.end()) {
        const auto node = reader.read_inode(existing->inode);
        if (!node) {
            result.error = "Could not load existing PFS file inode: " + reader.last_error();
            return result;
        }
        return replace_file(*node, path, bytes, options);
    }
    return create_file(*parent, path, name, bytes, options);
}

} // namespace ps2hdd::pfs
