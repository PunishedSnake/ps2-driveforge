#include "ps2hdd/pfs_write.hpp"

#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace ps2hdd::pfs {
namespace {

constexpr std::uint32_t kMetadataSectors =
    static_cast<std::uint32_t>(kMetadataSize / apa::kSectorSize);
constexpr std::uint64_t kMaxDirectoryBytes = 64ULL * 1024ULL * 1024ULL;

std::string normalize_path(std::string_view input)
{
    std::string out;
    out.reserve(input.size());
    bool slash = false;
    for (const char ch : input) {
        const char normalized = ch == '\\' ? '/' : ch;
        if (normalized == '/') {
            if (!slash) {
                out.push_back('/');
            }
            slash = true;
        } else {
            out.push_back(normalized);
            slash = false;
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

bool needs_directory_growth(std::string_view error) noexcept
{
    return error.find("no reusable dentry space") != std::string_view::npos;
}

bool needs_fragmented_allocation(std::string_view error) noexcept
{
    return error.find("No contiguous PFS") != std::string_view::npos ||
           error.find("No contiguous two-zone PFS") != std::string_view::npos;
}

std::uint64_t run_zones(std::span<const ImageWriter::ZoneRun> runs) noexcept
{
    std::uint64_t total = 0;
    for (const auto& run : runs) {
        total += run.count;
    }
    return total;
}

} // namespace

std::vector<ImageWriter::ZoneRun> ImageWriter::find_free_runs(
    std::uint32_t count, std::size_t preferred_subpart,
    std::uint32_t preferred_zone, std::size_t max_runs)
{
    std::vector<ZoneRun> runs;
    if (count == 0) {
        return runs;
    }
    if (max_runs == 0) {
        return {};
    }

    std::uint32_t remaining = count;
    auto append_sequence = [&](std::size_t subpart, std::uint32_t first,
                               std::uint32_t length) -> bool {
        while (length != 0 && remaining != 0) {
            if (runs.size() >= max_runs) {
                return false;
            }
            const auto take = std::min<std::uint32_t>(
                {length, remaining, std::numeric_limits<std::uint16_t>::max()});
            runs.push_back({subpart, first, take});
            first += take;
            length -= take;
            remaining -= take;
        }
        return remaining == 0;
    };

    auto scan_range = [&](std::size_t subpart, std::uint32_t begin,
                          std::uint32_t end) -> bool {
        std::uint32_t run_first = 0;
        std::uint32_t run_count = 0;
        auto flush = [&]() -> bool {
            if (run_count == 0) {
                return false;
            }
            const auto done = append_sequence(subpart, run_first, run_count);
            run_count = 0;
            return done;
        };

        for (std::uint32_t zone = begin; zone < end && remaining != 0; ++zone) {
            if (!zone_used(subpart, zone)) {
                if (run_count == 0) {
                    run_first = zone;
                }
                ++run_count;
                if (run_count == std::numeric_limits<std::uint16_t>::max()) {
                    if (flush()) {
                        return true;
                    }
                    if (runs.size() >= max_runs && remaining != 0) {
                        return false;
                    }
                }
            } else if (flush()) {
                return true;
            }
            if (!error_.empty()) {
                return false;
            }
        }
        return flush() || remaining == 0;
    };

    std::vector<std::size_t> order;
    if (preferred_subpart < writable_volume_.extent_count()) {
        order.push_back(preferred_subpart);
    }
    for (std::size_t subpart = 0; subpart < writable_volume_.extent_count(); ++subpart) {
        if (subpart != preferred_subpart) {
            order.push_back(subpart);
        }
    }

    for (const auto subpart : order) {
        const auto zones64 = zones_in_subpart(subpart);
        if (zones64 == 0 || zones64 > std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }
        const auto total = static_cast<std::uint32_t>(zones64);
        if (subpart == preferred_subpart && preferred_zone < total) {
            if (scan_range(subpart, preferred_zone, total)) {
                break;
            }
            if (remaining != 0 && preferred_zone != 0 &&
                scan_range(subpart, 0, preferred_zone)) {
                break;
            }
        } else if (scan_range(subpart, 0, total)) {
            break;
        }
        if (!error_.empty() || (runs.size() >= max_runs && remaining != 0)) {
            break;
        }
    }

    if (remaining != 0) {
        return {};
    }
    return runs;
}

bool ImageWriter::reserve_runs(std::span<const ZoneRun> runs,
                               FileWriteResult& result,
                               std::string_view label)
{
    std::vector<BitmapKey> dirty;
    std::string bitmap_error;
    for (const auto& run : runs) {
        for (std::uint32_t zone = 0; zone < run.count; ++zone) {
            if (!set_zone(run.subpart, run.first + zone, true, dirty, bitmap_error)) {
                result.error = bitmap_error;
                discard_bitmap_cache(dirty);
                return false;
            }
        }
    }
    return commit_bitmap_changes(dirty, result, label);
}

bool ImageWriter::release_runs(std::span<const ZoneRun> runs,
                               FileWriteResult& result,
                               std::string_view label)
{
    std::vector<BitmapKey> dirty;
    std::string bitmap_error;
    for (const auto& run : runs) {
        for (std::uint32_t zone = 0; zone < run.count; ++zone) {
            if (!set_zone(run.subpart, run.first + zone, false, dirty, bitmap_error)) {
                result.error = bitmap_error;
                discard_bitmap_cache(dirty);
                return false;
            }
        }
    }
    return commit_bitmap_changes(dirty, result, label);
}

bool ImageWriter::write_payload_runs(std::span<const ZoneRun> runs,
                                     std::span<const std::byte> bytes,
                                     std::size_t batch_bytes,
                                     FileWriteResult& result)
{
    if (bytes.empty()) {
        return true;
    }
    const auto zone_size = static_cast<std::uint64_t>(probe_.super.zone_size);
    const auto capacity = run_zones(runs) * zone_size;
    if (bytes.size() > capacity) {
        result.error = "PFS fragmented extent plan is smaller than payload";
        return false;
    }

    std::size_t batch = std::max<std::size_t>(apa::kSectorSize, batch_bytes);
    batch -= batch % apa::kSectorSize;
    std::size_t position = 0;

    for (const auto& run : runs) {
        if (position == bytes.size()) {
            break;
        }
        const auto run_capacity64 = static_cast<std::uint64_t>(run.count) * zone_size;
        const auto run_take = static_cast<std::size_t>(
            std::min<std::uint64_t>(run_capacity64, bytes.size() - position));
        const auto first_sector64 = static_cast<std::uint64_t>(run.first) * sectors_per_zone();
        if (first_sector64 > std::numeric_limits<std::uint32_t>::max()) {
            result.error = "PFS fragmented payload sector address overflow";
            return false;
        }

        std::size_t local = 0;
        std::uint32_t sector = static_cast<std::uint32_t>(first_sector64);
        while (local + apa::kSectorSize <= run_take) {
            const auto full = (run_take - local) / apa::kSectorSize * apa::kSectorSize;
            const auto take = std::min(batch, full);
            const auto sectors = static_cast<std::uint32_t>(take / apa::kSectorSize);
            if (!writable_volume_.write_sectors(
                    run.subpart, sector, sectors,
                    bytes.subspan(position + local, take))) {
                result.error = "PFS fragmented payload write failed";
                return false;
            }
            ++result.payload_write_calls;
            local += take;
            sector += sectors;
        }
        if (local < run_take) {
            std::array<std::byte, apa::kSectorSize> tail{};
            std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(position + local),
                      bytes.begin() + static_cast<std::ptrdiff_t>(position + run_take),
                      tail.begin());
            if (!writable_volume_.write_sectors(run.subpart, sector, 1, tail)) {
                result.error = "PFS fragmented payload tail write failed";
                return false;
            }
            ++result.payload_write_calls;
        }
        position += run_take;
    }

    if (position != bytes.size()) {
        result.error = "PFS fragmented payload planner did not cover the complete file";
        return false;
    }
    if (!writable_volume_.flush()) {
        result.error = "PFS fragmented payload flush failed";
        return false;
    }
    return true;
}

bool ImageWriter::verify_payload_runs(std::span<const ZoneRun> runs,
                                      std::span<const std::byte> bytes,
                                      std::size_t batch_bytes)
{
    if (bytes.empty()) {
        return true;
    }
    std::size_t batch = std::max<std::size_t>(apa::kSectorSize, batch_bytes);
    batch -= batch % apa::kSectorSize;
    std::vector<std::byte> buffer(batch);
    std::size_t position = 0;
    const auto zone_size = static_cast<std::uint64_t>(probe_.super.zone_size);

    for (const auto& run : runs) {
        if (position == bytes.size()) {
            break;
        }
        const auto run_capacity64 = static_cast<std::uint64_t>(run.count) * zone_size;
        const auto run_take = static_cast<std::size_t>(
            std::min<std::uint64_t>(run_capacity64, bytes.size() - position));
        const auto first_sector64 = static_cast<std::uint64_t>(run.first) * sectors_per_zone();
        if (first_sector64 > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        std::uint32_t sector = static_cast<std::uint32_t>(first_sector64);
        std::size_t local = 0;
        while (local < run_take) {
            const auto payload_take = std::min(batch, run_take - local);
            const auto sectors = static_cast<std::uint32_t>(
                (payload_take + apa::kSectorSize - 1U) / apa::kSectorSize);
            const auto disk_take = static_cast<std::size_t>(sectors) * apa::kSectorSize;
            if (buffer.size() < disk_take) {
                buffer.resize(disk_take);
            }
            if (!writable_volume_.read_sectors(
                    run.subpart, sector, sectors,
                    std::span<std::byte>(buffer.data(), disk_take))) {
                return false;
            }
            if (!std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(position + local),
                            bytes.begin() + static_cast<std::ptrdiff_t>(position + local + payload_take),
                            buffer.begin())) {
                return false;
            }
            local += payload_take;
            sector += sectors;
        }
        position += run_take;
    }
    return position == bytes.size();
}

bool ImageWriter::grow_directory(Node& directory, FileWriteResult& result)
{
    if ((directory.inode.mode & kModeMask) != kModeDirectory) {
        result.error = "PFS directory growth target is not a directory";
        return false;
    }
    if (directory.inode.number_data <= 1 || directory.inode.number_data > kInodeMaxBlocks ||
        directory.inode.next_segment.number != 0 || directory.inode.number_segdesg != 1) {
        result.error = "PFS directory growth requires a direct SEGD layout";
        return false;
    }
    if (directory.inode.size == 0 || directory.inode.size > kMaxDirectoryBytes - apa::kSectorSize ||
        (directory.inode.size % apa::kSectorSize) != 0) {
        result.error = "PFS directory size is invalid or cannot grow by one sector";
        return false;
    }

    std::uint64_t capacity = 0;
    for (std::size_t index = 1; index < directory.inode.number_data; ++index) {
        const auto& block = directory.inode.data[index];
        if (block.count == 0 || block.subpart >= writable_volume_.extent_count()) {
            result.error = "PFS directory has an invalid direct data extent";
            return false;
        }
        capacity += static_cast<std::uint64_t>(block.count) * probe_.super.zone_size;
    }
    if (directory.inode.size > capacity) {
        result.error = "PFS directory size exceeds its allocated data capacity";
        return false;
    }

    Inode grown = directory.inode;
    std::size_t target_subpart = 0;
    std::uint32_t target_sector = 0;
    ZoneRun allocated{};
    bool allocated_new_zone = false;

    if (directory.inode.size < capacity) {
        std::uint64_t logical = 0;
        bool mapped = false;
        for (std::size_t index = 1; index < directory.inode.number_data; ++index) {
            const auto& block = directory.inode.data[index];
            const auto bytes = static_cast<std::uint64_t>(block.count) * probe_.super.zone_size;
            if (directory.inode.size >= logical && directory.inode.size < logical + bytes) {
                const auto within = directory.inode.size - logical;
                const auto sector64 = static_cast<std::uint64_t>(block.number) * sectors_per_zone() +
                                      within / apa::kSectorSize;
                if ((within % apa::kSectorSize) != 0 ||
                    sector64 > std::numeric_limits<std::uint32_t>::max()) {
                    result.error = "PFS directory growth sector mapping is invalid";
                    return false;
                }
                target_subpart = block.subpart;
                target_sector = static_cast<std::uint32_t>(sector64);
                mapped = true;
                break;
            }
            logical += bytes;
        }
        if (!mapped) {
            result.error = "PFS directory growth could not map existing spare capacity";
            return false;
        }
    } else {
        const auto& last = directory.inode.data[directory.inode.number_data - 1U];
        const auto preferred_zone64 = static_cast<std::uint64_t>(last.number) + last.count;
        const auto preferred_zone = preferred_zone64 <= std::numeric_limits<std::uint32_t>::max()
                                        ? static_cast<std::uint32_t>(preferred_zone64)
                                        : last.number;
        allocated = find_free_run(1, last.subpart, preferred_zone);
        if (allocated.count != 1 || allocated.subpart > 0xFFFFU) {
            result.error = error_.empty() ? "No free PFS zone is available for directory growth"
                                          : error_;
            return false;
        }
        if (!reserve_runs(std::span<const ZoneRun>(&allocated, 1), result,
                          "reserve PFS directory growth zone")) {
            return false;
        }
        allocated_new_zone = true;

        auto& grown_last = grown.data[grown.number_data - 1U];
        if (grown_last.subpart == allocated.subpart &&
            static_cast<std::uint64_t>(grown_last.number) + grown_last.count == allocated.first &&
            grown_last.count < std::numeric_limits<std::uint16_t>::max()) {
            ++grown_last.count;
        } else {
            if (grown.number_data >= kInodeMaxBlocks) {
                const auto saved = result.error;
                FileWriteResult cleanup;
                if (!release_runs(std::span<const ZoneRun>(&allocated, 1), cleanup,
                                  "release refused PFS directory growth zone")) {
                    result.warning = "Directory growth needs SEGI and the reserved zone could not be released: " +
                                     cleanup.error;
                }
                result.error = saved.empty() ? "PFS directory growth would require a SEGI descriptor"
                                             : saved;
                return false;
            }
            grown.data[grown.number_data++] = {
                allocated.first, static_cast<std::uint16_t>(allocated.subpart), 1};
        }
        ++grown.number_blocks;
        const auto sector64 = static_cast<std::uint64_t>(allocated.first) * sectors_per_zone();
        if (sector64 > std::numeric_limits<std::uint32_t>::max()) {
            result.error = "PFS directory growth sector address overflow";
            FileWriteResult cleanup;
            (void)release_runs(std::span<const ZoneRun>(&allocated, 1), cleanup,
                               "release overflowed PFS directory growth zone");
            return false;
        }
        target_subpart = allocated.subpart;
        target_sector = static_cast<std::uint32_t>(sector64);
    }

    std::array<std::byte, apa::kSectorSize> empty_sector{};
    store_u16(empty_sector.data() + 6, static_cast<std::uint16_t>(apa::kSectorSize));
    grown.size += apa::kSectorSize;
    grown.checksum = inode_checksum(grown);

    const auto inode_block64 = static_cast<std::uint64_t>(grown.inode_block.number) << inode_scale();
    const auto inode_sector64 = inode_block64 * kMetadataSectors;
    std::uint64_t inode_offset = 0;
    std::uint64_t data_offset = 0;
    if (inode_sector64 > std::numeric_limits<std::uint32_t>::max() ||
        !writable_volume_.absolute_byte_offset(grown.inode_block.subpart,
                                               static_cast<std::uint32_t>(inode_sector64),
                                               kMetadataSectors, inode_offset) ||
        !writable_volume_.absolute_byte_offset(target_subpart, target_sector, 1, data_offset)) {
        result.error = "PFS directory growth maps outside its APA extent";
        if (allocated_new_zone) {
            FileWriteResult cleanup;
            (void)release_runs(std::span<const ZoneRun>(&allocated, 1), cleanup,
                               "release unmappable PFS directory growth zone");
        }
        return false;
    }

    WriteTransaction transaction(device_);
    if (!transaction.stage(data_offset, empty_sector, "initialize grown PFS directory sector") ||
        !transaction.stage(inode_offset, std::as_bytes(std::span{&grown, 1}),
                           "publish grown PFS directory inode")) {
        result.error = "Could not stage PFS directory growth: " + transaction.stage_error();
        if (allocated_new_zone) {
            FileWriteResult cleanup;
            (void)release_runs(std::span<const ZoneRun>(&allocated, 1), cleanup,
                               "release unstaged PFS directory growth zone");
        }
        return false;
    }

    const auto committed = transaction.commit([&]() -> std::string {
        Reader verify(read_volume_);
        const auto node = verify.read_inode(directory.location);
        if (!node || node->inode.size != grown.size) {
            return "PFS reader did not observe grown directory inode";
        }
        (void)verify.list_directory(*node, true);
        return verify.last_error();
    });
    if (!committed.ok) {
        result.error = "PFS directory growth transaction failed: " + committed.error;
        if (allocated_new_zone) {
            FileWriteResult cleanup;
            if (!release_runs(std::span<const ZoneRun>(&allocated, 1), cleanup,
                              "release rolled-back PFS directory growth zone")) {
                result.warning = "Directory growth rolled back but the new zone remains allocated: " +
                                 cleanup.error;
            }
        }
        return false;
    }

    ++result.metadata_transactions;
    directory.inode = grown;
    return true;
}

ImageWriter::DentryLocation ImageWriter::find_dentry(const Node& parent,
                                                      std::string_view name)
{
    DentryLocation result;
    if ((parent.inode.mode & kModeMask) != kModeDirectory ||
        parent.inode.number_data <= 1 || parent.inode.number_data > kInodeMaxBlocks ||
        parent.inode.next_segment.number != 0) {
        result.error = "PFS dentry lookup requires a direct directory layout";
        return result;
    }

    Reader reader(read_volume_, probe_);
    std::vector<std::byte> directory(static_cast<std::size_t>(parent.inode.size));
    if (!reader.read(parent, 0, directory)) {
        result.error = "Could not read PFS parent directory for unlink: " + reader.last_error();
        return result;
    }

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
        if (allocated < 8 || (allocated & 3U) != 0 || position + allocated > sector_end ||
            path_len > allocated - 8U) {
            result.error = "Malformed PFS dentry while planning unlink";
            return result;
        }

        if (inode != 0 && path_len == name.size() &&
            std::memcmp(ptr + 8, name.data(), name.size()) == 0) {
            const auto logical_sector = (position / apa::kSectorSize) * apa::kSectorSize;
            std::uint64_t logical = 0;
            for (std::size_t index = 1; index < parent.inode.number_data; ++index) {
                const auto& block = parent.inode.data[index];
                const auto extent_bytes =
                    static_cast<std::uint64_t>(block.count) * probe_.super.zone_size;
                if (logical_sector >= logical && logical_sector < logical + extent_bytes) {
                    const auto within = logical_sector - logical;
                    const auto sector64 = static_cast<std::uint64_t>(block.number) * sectors_per_zone() +
                                          within / apa::kSectorSize;
                    if ((within % apa::kSectorSize) != 0 ||
                        sector64 > std::numeric_limits<std::uint32_t>::max()) {
                        result.error = "PFS unlink dentry sector mapping is invalid";
                        return result;
                    }
                    result.subpart = block.subpart;
                    result.sector = static_cast<std::uint32_t>(sector64);
                    result.offset_in_sector = position % apa::kSectorSize;
                    result.allocated = static_cast<std::uint16_t>(allocated);
                    if (!writable_volume_.read_sectors(result.subpart, result.sector, 1,
                                                       result.sector_bytes)) {
                        result.error = "Could not read physical PFS dentry sector for unlink";
                        return result;
                    }
                    result.ok = true;
                    return result;
                }
                logical += extent_bytes;
            }
            result.error = "PFS unlink could not map dentry to a data extent";
            return result;
        }
        position += allocated;
    }

    result.error = "PFS directory entry was not found for unlink";
    return result;
}

FileWriteResult ImageWriter::create_file_fragmented(
    Node parent, std::string_view full_path, std::string_view name,
    std::span<const std::byte> bytes, const FileWriteOptions& options)
{
    FileWriteResult result;
    result.disposition = FileWriteDisposition::created;
    result.path = std::string(full_path);
    result.bytes = bytes.size();

    if ((options.mode & kModeMask) != kModeRegular) {
        result.error = "PFS file mode must describe a regular file";
        return result;
    }

    auto slot_probe = plan_dentry_insert(parent, name, {1, 0, 1});
    if (!slot_probe.ok && needs_directory_growth(slot_probe.error)) {
        if (!grow_directory(parent, result)) {
            return result;
        }
        slot_probe = plan_dentry_insert(parent, name, {1, 0, 1});
    }
    if (!slot_probe.ok) {
        result.error = slot_probe.error;
        return result;
    }

    const auto zone_size = static_cast<std::uint64_t>(probe_.super.zone_size);
    const auto data_zones64 = (bytes.size() + zone_size - 1U) / zone_size;
    if (data_zones64 > std::numeric_limits<std::uint32_t>::max() - 1U) {
        result.error = "PFS file is too large for bounded direct-extent allocation";
        return result;
    }
    const auto total_zones = static_cast<std::uint32_t>(data_zones64) + 1U;
    auto allocation = find_free_runs(total_zones, parent.location.subpart,
                                     parent.location.number, kInodeMaxBlocks - 1U);
    if (allocation.empty() || run_zones(allocation) != total_zones) {
        result.error = error_.empty()
                           ? "PFS fragmented allocator cannot represent file without SEGI"
                           : error_;
        return result;
    }
    if (allocation.front().subpart > 0xFFU) {
        result.error = "PFS inode subpart cannot be represented in a directory entry";
        return result;
    }

    const BlockInfo inode_location{allocation.front().first,
                                   static_cast<std::uint16_t>(allocation.front().subpart), 1};
    std::vector<ZoneRun> data_runs = allocation;
    if (data_runs.front().count == 1) {
        data_runs.erase(data_runs.begin());
    } else {
        ++data_runs.front().first;
        --data_runs.front().count;
    }
    if (data_runs.size() > kInodeMaxBlocks - 1U) {
        result.error = "PFS fragmented file needs an indirect SEGI descriptor";
        return result;
    }

    const auto dentry = plan_dentry_insert(parent, name, inode_location);
    if (!dentry.ok) {
        result.error = dentry.error;
        return result;
    }
    if (!reserve_runs(allocation, result, "reserve fragmented PFS file zones")) {
        return result;
    }

    const auto cleanup = [&]() {
        const auto saved_error = result.error;
        FileWriteResult release;
        if (!release_runs(allocation, release, "release abandoned fragmented PFS file zones")) {
            result.warning = "Fragmented file create failed safely but reserved zones remain allocated: " +
                             release.error;
        }
        result.error = saved_error;
    };

    if (!write_payload_runs(data_runs, bytes, options.io_batch_bytes, result) ||
        !verify_payload_runs(data_runs, bytes, options.io_batch_bytes)) {
        if (result.error.empty()) {
            result.error = "PFS fragmented payload verification failed";
        }
        cleanup();
        return result;
    }

    Inode inode{};
    inode.magic = kSegdMagic;
    inode.inode_block = inode_location;
    inode.last_segment = inode_location;
    inode.data[0] = inode_location;
    for (std::size_t index = 0; index < data_runs.size(); ++index) {
        inode.data[index + 1U] = {data_runs[index].first,
                                  static_cast<std::uint16_t>(data_runs[index].subpart),
                                  static_cast<std::uint16_t>(data_runs[index].count)};
    }
    inode.mode = options.mode;
    inode.uid = options.uid;
    inode.gid = options.gid;
    inode.atime = options.timestamp;
    inode.ctime = options.timestamp;
    inode.mtime = options.timestamp;
    inode.size = bytes.size();
    inode.number_blocks = total_zones;
    inode.number_data = static_cast<std::uint32_t>(1U + data_runs.size());
    inode.number_segdesg = 1;
    inode.subpart = static_cast<std::uint32_t>(inode_location.subpart);
    inode.checksum = inode_checksum(inode);

    const auto inode_block64 = static_cast<std::uint64_t>(inode_location.number) << inode_scale();
    const auto inode_sector64 = inode_block64 * kMetadataSectors;
    std::uint64_t inode_offset = 0;
    std::uint64_t dentry_offset = 0;
    if (inode_sector64 > std::numeric_limits<std::uint32_t>::max() ||
        !writable_volume_.absolute_byte_offset(inode_location.subpart,
                                               static_cast<std::uint32_t>(inode_sector64),
                                               kMetadataSectors, inode_offset) ||
        !writable_volume_.absolute_byte_offset(dentry.subpart, dentry.sector, 1, dentry_offset)) {
        result.error = "PFS fragmented file publication maps outside APA extent";
        cleanup();
        return result;
    }

    WriteTransaction publication(device_);
    if (!publication.stage(inode_offset, std::as_bytes(std::span{&inode, 1}),
                           "publish fragmented PFS inode") ||
        !publication.stage(dentry_offset, dentry.sector_bytes,
                           "publish fragmented PFS dentry")) {
        result.error = "Could not stage fragmented PFS publication: " + publication.stage_error();
        cleanup();
        return result;
    }
    const auto committed = publication.commit([&]() -> std::string {
        return verify_file(full_path, bytes) ? std::string{}
                                             : std::string{"PFS reader did not verify fragmented file"};
    });
    if (!committed.ok) {
        result.error = "Fragmented PFS publication failed: " + committed.error;
        cleanup();
        return result;
    }
    ++result.metadata_transactions;
    result.ok = true;
    return result;
}

FileWriteResult ImageWriter::replace_file_fragmented(
    const Node& node, std::string_view full_path,
    std::span<const std::byte> bytes, const FileWriteOptions& options)
{
    FileWriteResult result;
    result.disposition = FileWriteDisposition::replaced;
    result.path = std::string(full_path);
    result.bytes = bytes.size();

    if ((node.inode.mode & kModeMask) != kModeRegular ||
        node.inode.number_data == 0 || node.inode.number_data > kInodeMaxBlocks ||
        node.inode.next_segment.number != 0 || node.inode.number_segdesg != 1) {
        result.error = "PFS fragmented replacement currently requires a direct regular-file layout";
        return result;
    }

    std::vector<ZoneRun> old_runs;
    for (std::size_t index = 1; index < node.inode.number_data; ++index) {
        const auto& block = node.inode.data[index];
        if (block.count == 0 || block.subpart >= writable_volume_.extent_count()) {
            result.error = "PFS replacement target contains an invalid data extent";
            return result;
        }
        old_runs.push_back({block.subpart, block.number, block.count});
    }

    const auto zone_size = static_cast<std::uint64_t>(probe_.super.zone_size);
    const auto data_zones64 = (bytes.size() + zone_size - 1U) / zone_size;
    if (data_zones64 > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "PFS replacement is too large for bounded direct extents";
        return result;
    }
    const auto data_zones = static_cast<std::uint32_t>(data_zones64);
    std::vector<ZoneRun> new_runs;
    if (data_zones != 0) {
        const auto preferred_sub = old_runs.empty() ? node.location.subpart : old_runs.front().subpart;
        const auto preferred_zone = old_runs.empty() ? node.location.number : old_runs.front().first;
        new_runs = find_free_runs(data_zones, preferred_sub, preferred_zone,
                                  kInodeMaxBlocks - 1U);
        if (new_runs.empty() || run_zones(new_runs) != data_zones) {
            result.error = error_.empty()
                               ? "PFS fragmented replacement would require a SEGI descriptor"
                               : error_;
            return result;
        }
        if (!reserve_runs(new_runs, result, "reserve fragmented PFS replacement zones")) {
            return result;
        }
        if (!write_payload_runs(new_runs, bytes, options.io_batch_bytes, result) ||
            !verify_payload_runs(new_runs, bytes, options.io_batch_bytes)) {
            if (result.error.empty()) {
                result.error = "PFS fragmented replacement payload verification failed";
            }
            const auto saved_error = result.error;
            FileWriteResult cleanup;
            if (!release_runs(new_runs, cleanup, "release failed fragmented replacement zones")) {
                result.warning = "Replacement failed before publication but new zones remain allocated: " +
                                 cleanup.error;
            }
            result.error = saved_error;
            return result;
        }
    }

    Inode replacement = node.inode;
    replacement.next_segment = {};
    replacement.last_segment = replacement.inode_block;
    for (std::size_t index = 1; index < kInodeMaxBlocks; ++index) {
        replacement.data[index] = {};
    }
    for (std::size_t index = 0; index < new_runs.size(); ++index) {
        replacement.data[index + 1U] = {
            new_runs[index].first, static_cast<std::uint16_t>(new_runs[index].subpart),
            static_cast<std::uint16_t>(new_runs[index].count)};
    }
    replacement.size = bytes.size();
    replacement.number_blocks = 1U + data_zones;
    replacement.number_data = static_cast<std::uint32_t>(1U + new_runs.size());
    replacement.number_segdesg = 1;
    replacement.atime = options.timestamp;
    replacement.mtime = options.timestamp;
    replacement.checksum = inode_checksum(replacement);

    const auto inode_block64 = static_cast<std::uint64_t>(replacement.inode_block.number) << inode_scale();
    const auto inode_sector64 = inode_block64 * kMetadataSectors;
    std::uint64_t inode_offset = 0;
    if (inode_sector64 > std::numeric_limits<std::uint32_t>::max() ||
        !writable_volume_.absolute_byte_offset(replacement.inode_block.subpart,
                                               static_cast<std::uint32_t>(inode_sector64),
                                               kMetadataSectors, inode_offset)) {
        result.error = "PFS fragmented replacement inode maps outside APA extent";
        return result;
    }

    WriteTransaction publication(device_);
    if (!publication.stage(inode_offset, std::as_bytes(std::span{&replacement, 1}),
                           "switch PFS inode to fragmented replacement")) {
        result.error = "Could not stage fragmented replacement inode: " + publication.stage_error();
        return result;
    }
    const auto committed = publication.commit([&]() -> std::string {
        return verify_file(full_path, bytes) ? std::string{}
                                             : std::string{"PFS reader did not verify fragmented replacement"};
    });
    if (!committed.ok) {
        result.error = "PFS fragmented replacement publication failed: " + committed.error;
        if (!new_runs.empty()) {
            const auto saved_error = result.error;
            FileWriteResult cleanup;
            if (!release_runs(new_runs, cleanup, "release rolled-back fragmented replacement zones")) {
                result.warning = "Replacement rolled back but new zones remain allocated: " + cleanup.error;
            }
            result.error = saved_error;
        }
        return result;
    }
    ++result.metadata_transactions;

    if (!old_runs.empty()) {
        const auto saved_error = result.error;
        FileWriteResult release;
        if (!release_runs(old_runs, release, "release old fragmented PFS file zones")) {
            result.warning = "Replacement is valid but old PFS zones remain allocated: " + release.error;
            result.error = saved_error;
            result.ok = true;
            return result;
        }
        result.metadata_transactions += release.metadata_transactions;
        result.bitmap_chunks_touched += release.bitmap_chunks_touched;
    }

    result.ok = true;
    return result;
}

DirectoryEnsureResult ImageWriter::ensure_directory_full(
    std::string_view input_path, const DirectoryWriteOptions& options)
{
    DirectoryEnsureResult result;
    if (!valid()) {
        result.error = error_.empty() ? "PFS writer session is invalid" : error_;
        return result;
    }
    if ((options.mode & kModeMask) != kModeDirectory) {
        result.error = "PFS mkdir mode must describe a directory";
        return result;
    }

    const auto path = normalize_path(input_path);
    result.path = path.empty() ? "/" : path;
    if (contains_parent_escape(path)) {
        result.error = "PFS mkdir path cannot contain '..' traversal";
        return result;
    }
    if (path.empty() || path == "/") {
        result.ok = true;
        return result;
    }

    std::string parent_path = "/";
    std::size_t cursor = path.front() == '/' ? 1U : 0U;
    while (cursor <= path.size()) {
        const auto slash = path.find('/', cursor);
        const auto end = slash == std::string::npos ? path.size() : slash;
        const auto component = path.substr(cursor, end - cursor);
        if (component.empty() || component == "." || component == ".." || component.size() > 255) {
            result.error = "PFS mkdir path contains an invalid component";
            return result;
        }

        Reader reader(read_volume_, probe_);
        auto parent = reader.resolve(parent_path);
        if (!parent || (parent->inode.mode & kModeMask) != kModeDirectory) {
            result.error = "PFS mkdir parent does not resolve as a directory: " + parent_path;
            return result;
        }
        const auto entries = reader.list_directory(*parent, true);
        if (!reader.last_error().empty()) {
            result.error = "Could not enumerate PFS mkdir parent: " + reader.last_error();
            return result;
        }
        const auto existing = std::find_if(entries.begin(), entries.end(), [&](const DirectoryEntry& entry) {
            return entry.name == component;
        });
        const std::string full_path = parent_path == "/" ? "/" + component
                                                           : parent_path + "/" + component;

        if (existing != entries.end()) {
            const auto node = reader.read_inode(existing->inode);
            if (!node || (node->inode.mode & kModeMask) != kModeDirectory) {
                result.error = "PFS mkdir component exists and is not a directory: " + full_path;
                return result;
            }
        } else {
            auto created = create_directory(*parent, full_path, component, options);
            if (!created.ok && needs_directory_growth(created.error)) {
                FileWriteResult growth;
                if (!grow_directory(*parent, growth)) {
                    result.error = growth.error;
                    result.warning = growth.warning;
                    result.metadata_transactions += growth.metadata_transactions;
                    result.bitmap_chunks_touched += growth.bitmap_chunks_touched;
                    return result;
                }
                result.metadata_transactions += growth.metadata_transactions;
                result.bitmap_chunks_touched += growth.bitmap_chunks_touched;
                created = create_directory(*parent, full_path, component, options);
            }
            result.metadata_transactions += created.metadata_transactions;
            result.bitmap_chunks_touched += created.bitmap_chunks_touched;
            result.created_components += created.created_components;
            if (!created.warning.empty()) {
                result.warning = created.warning;
            }
            if (!created.ok) {
                result.error = created.error;
                return result;
            }
        }

        parent_path = full_path;
        if (slash == std::string::npos) {
            break;
        }
        cursor = slash + 1U;
    }

    result.ok = true;
    result.path = parent_path;
    return result;
}

FileWriteResult ImageWriter::write_file_full(std::string_view input_path,
                                             std::span<const std::byte> bytes,
                                             const FileWriteOptions& options)
{
    auto first = write_file(input_path, bytes, options);
    if (first.ok) {
        return first;
    }

    const auto path = normalize_path(input_path);
    if (path.empty() || path == "/" || contains_parent_escape(path)) {
        return first;
    }
    const auto slash = path.find_last_of('/');
    const std::string parent_path = slash == std::string::npos || slash == 0
                                        ? "/"
                                        : path.substr(0, slash);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1U);

    if (needs_directory_growth(first.error)) {
        Reader reader(read_volume_, probe_);
        auto parent = reader.resolve(parent_path);
        if (!parent) {
            return first;
        }
        FileWriteResult growth;
        if (!grow_directory(*parent, growth)) {
            first.warning = growth.warning;
            first.error = growth.error;
            first.metadata_transactions += growth.metadata_transactions;
            first.bitmap_chunks_touched += growth.bitmap_chunks_touched;
            return first;
        }
        auto retried = write_file(path, bytes, options);
        retried.metadata_transactions += growth.metadata_transactions;
        retried.bitmap_chunks_touched += growth.bitmap_chunks_touched;
        if (retried.ok || !needs_fragmented_allocation(retried.error)) {
            return retried;
        }
        first = std::move(retried);
    }

    if (!needs_fragmented_allocation(first.error)) {
        return first;
    }

    Reader reader(read_volume_, probe_);
    const auto existing = reader.resolve(path);
    if (existing) {
        auto result = replace_file_fragmented(*existing, path, bytes, options);
        result.metadata_transactions += first.metadata_transactions;
        result.bitmap_chunks_touched += first.bitmap_chunks_touched;
        return result;
    }

    auto parent = reader.resolve(parent_path);
    if (!parent || (parent->inode.mode & kModeMask) != kModeDirectory) {
        return first;
    }
    auto result = create_file_fragmented(*parent, path, name, bytes, options);
    result.metadata_transactions += first.metadata_transactions;
    result.bitmap_chunks_touched += first.bitmap_chunks_touched;
    return result;
}

FileRemoveResult ImageWriter::remove_file(std::string_view input_path)
{
    FileRemoveResult result;
    if (!valid()) {
        result.error = error_.empty() ? "PFS writer session is invalid" : error_;
        return result;
    }

    const auto path = normalize_path(input_path);
    result.path = path;
    if (path.empty() || path == "/" || contains_parent_escape(path)) {
        result.error = "PFS file unlink requires a normal non-root path";
        return result;
    }
    const auto slash = path.find_last_of('/');
    const std::string parent_path = slash == std::string::npos || slash == 0
                                        ? "/"
                                        : path.substr(0, slash);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1U);

    Reader reader(read_volume_, probe_);
    const auto parent = reader.resolve(parent_path);
    if (!parent || (parent->inode.mode & kModeMask) != kModeDirectory) {
        result.error = "PFS unlink parent directory was not found";
        return result;
    }
    const auto entries = reader.list_directory(*parent, true);
    if (!reader.last_error().empty()) {
        result.error = "Could not enumerate PFS unlink parent: " + reader.last_error();
        return result;
    }
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const DirectoryEntry& candidate) {
        return candidate.name == name;
    });
    if (entry == entries.end()) {
        result.error = "PFS unlink target does not exist";
        return result;
    }
    const auto node = reader.read_inode(entry->inode);
    if (!node || (node->inode.mode & kModeMask) != kModeRegular) {
        result.error = "PFS unlink target is not a regular file";
        return result;
    }
    if (node->inode.number_data == 0 || node->inode.number_data > kInodeMaxBlocks ||
        node->inode.next_segment.number != 0 || node->inode.number_segdesg != 1) {
        result.error = "PFS unlink currently requires a direct regular-file layout";
        return result;
    }

    std::vector<ZoneRun> release;
    release.push_back({node->inode.inode_block.subpart, node->inode.inode_block.number, 1});
    std::unordered_set<std::uint64_t> seen;
    auto remember = [&](const ZoneRun& run) -> bool {
        for (std::uint32_t i = 0; i < run.count; ++i) {
            const auto key = (static_cast<std::uint64_t>(run.subpart) << 32U) | (run.first + i);
            if (!seen.insert(key).second || !zone_used(run.subpart, run.first + i)) {
                return false;
            }
        }
        return true;
    };
    if (!remember(release.front())) {
        result.error = "PFS unlink inode zone is duplicated or bitmap-free";
        return result;
    }
    for (std::size_t index = 1; index < node->inode.number_data; ++index) {
        const auto& block = node->inode.data[index];
        ZoneRun run{block.subpart, block.number, block.count};
        if (run.count == 0 || !remember(run)) {
            result.error = "PFS unlink data extent is duplicated, invalid or bitmap-free";
            return result;
        }
        release.push_back(run);
    }

    auto dentry = find_dentry(*parent, name);
    if (!dentry.ok) {
        result.error = dentry.error;
        return result;
    }
    auto* target = dentry.sector_bytes.data() + static_cast<std::ptrdiff_t>(dentry.offset_in_sector);
    std::fill(target, target + dentry.allocated, std::byte{0});
    store_u16(target + 6, dentry.allocated);

    std::uint64_t dentry_offset = 0;
    if (!writable_volume_.absolute_byte_offset(dentry.subpart, dentry.sector, 1, dentry_offset)) {
        result.error = "PFS unlink dentry sector maps outside APA extent";
        return result;
    }

    WriteTransaction unlink(device_);
    if (!unlink.stage(dentry_offset, dentry.sector_bytes, "unlink PFS directory entry")) {
        result.error = "Could not stage PFS directory unlink: " + unlink.stage_error();
        return result;
    }
    const auto committed = unlink.commit([&]() -> std::string {
        Reader verify(read_volume_);
        if (verify.resolve(path)) {
            return "PFS path still resolves after unlink";
        }
        const auto verify_parent = verify.resolve(parent_path);
        if (!verify_parent) {
            return "PFS parent became unreadable after unlink";
        }
        (void)verify.list_directory(*verify_parent, true);
        return verify.last_error();
    });
    if (!committed.ok) {
        result.error = "PFS directory unlink transaction failed: " + committed.error;
        return result;
    }
    ++result.metadata_transactions;

    FileWriteResult release_stats;
    if (!release_runs(release, release_stats, "release unlinked PFS file zones")) {
        result.warning = "File is unlinked and no longer visible, but its zones remain allocated: " +
                         release_stats.error;
        result.ok = true;
        return result;
    }
    result.metadata_transactions += release_stats.metadata_transactions;
    result.bitmap_chunks_touched += release_stats.bitmap_chunks_touched;
    result.bytes_freed = run_zones(release) * probe_.super.zone_size;
    result.ok = true;
    return result;
}

} // namespace ps2hdd::pfs
