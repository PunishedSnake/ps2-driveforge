#include "ps2hdd/pfs_write.hpp"

#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace ps2hdd::pfs {
namespace {

constexpr std::uint32_t kMetadataSectors =
    static_cast<std::uint32_t>(kMetadataSize / apa::kSectorSize);

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

void store_u16(std::byte* target, std::uint16_t value) noexcept
{
    std::memcpy(target, &value, sizeof(value));
}

void store_u32(std::byte* target, std::uint32_t value) noexcept
{
    std::memcpy(target, &value, sizeof(value));
}

void put_dentry(std::span<std::byte> sector, std::size_t offset,
                const BlockInfo& inode, std::string_view name,
                std::uint16_t mode, std::uint16_t allocated)
{
    store_u32(sector.data() + static_cast<std::ptrdiff_t>(offset), inode.number);
    sector[offset + 4] = static_cast<std::byte>(inode.subpart);
    sector[offset + 5] = static_cast<std::byte>(name.size());
    store_u16(sector.data() + static_cast<std::ptrdiff_t>(offset + 6),
              static_cast<std::uint16_t>((mode & kModeMask) | allocated));
    std::memcpy(sector.data() + static_cast<std::ptrdiff_t>(offset + 8),
                name.data(), name.size());
}

bool same_block(const BlockInfo& left, const BlockInfo& right) noexcept
{
    return left.number == right.number && left.subpart == right.subpart;
}

} // namespace

ImageWriter::DirectorySlot ImageWriter::plan_directory_dentry_insert(
    const Node& parent, std::string_view name, const BlockInfo& inode_location)
{
    DirectorySlot slot;
    if ((parent.inode.mode & kModeMask) != kModeDirectory) {
        slot.error = "PFS parent is not a directory";
        return slot;
    }
    if (name.empty() || name.size() > 255) {
        slot.error = "PFS directory name must contain 1..255 bytes";
        return slot;
    }
    if (parent.inode.number_data <= 1 || parent.inode.number_data > kInodeMaxBlocks ||
        parent.inode.number_segdesg != 1 || parent.inode.next_segment.number != 0) {
        slot.error = "PFS parent directory uses an indirect/unsupported data layout";
        return slot;
    }
    if (parent.inode.size == 0 || parent.inode.size > 64ULL * 1024ULL * 1024ULL) {
        slot.error = "PFS parent directory size is invalid for bounded writer";
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
            slot.error = "Malformed PFS directory entry while planning mkdir";
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
        slot.error = "PFS parent has no reusable dentry space; directory growth is not enabled yet";
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
            const auto sector64 = static_cast<std::uint64_t>(block.number) * sectors_per_zone() +
                                  within / apa::kSectorSize;
            if ((within % apa::kSectorSize) != 0 ||
                sector64 > std::numeric_limits<std::uint32_t>::max()) {
                slot.error = "PFS parent directory sector address is invalid";
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
        slot.error = "Could not map/read PFS parent directory sector for mkdir";
        return slot;
    }

    const auto in_sector = chosen_position % apa::kSectorSize;
    auto* ptr = slot.sector_bytes.data() + static_cast<std::ptrdiff_t>(in_sector);
    const auto old_raw = load_u16(ptr + 6);
    const auto old_allocated = static_cast<std::size_t>(old_raw & 0x0FFFU);
    std::size_t new_position = in_sector;
    std::size_t new_allocated = old_allocated;

    if (!reuse_empty) {
        store_u16(ptr + 6, static_cast<std::uint16_t>(existing_mode | existing_actual));
        new_position += existing_actual;
        new_allocated -= existing_actual;
    }

    auto* target = slot.sector_bytes.data() + static_cast<std::ptrdiff_t>(new_position);
    std::fill(target, target + static_cast<std::ptrdiff_t>(new_allocated), std::byte{0});
    store_u32(target, inode_location.number);
    target[4] = static_cast<std::byte>(inode_location.subpart);
    target[5] = static_cast<std::byte>(name.size());
    store_u16(target + 6, static_cast<std::uint16_t>(kModeDirectory | new_allocated));
    std::memcpy(target + 8, name.data(), name.size());
    slot.ok = true;
    return slot;
}

DirectoryEnsureResult ImageWriter::create_directory(const Node& parent,
                                                    std::string_view full_path,
                                                    std::string_view name,
                                                    const DirectoryWriteOptions& options)
{
    DirectoryEnsureResult result;
    result.path = std::string(full_path);

    if ((options.mode & kModeMask) != kModeDirectory) {
        result.error = "PFS mkdir mode must describe a directory";
        return result;
    }

    const auto preferred_sub = static_cast<std::size_t>(parent.inode.inode_block.subpart);
    const auto run = find_free_run(2, preferred_sub, parent.inode.inode_block.number);
    if (run.count != 2 || run.subpart > 0xFFU) {
        result.error = error_.empty() ? "No contiguous two-zone PFS run is available for directory"
                                      : error_;
        return result;
    }

    const BlockInfo inode_location{run.first, static_cast<std::uint16_t>(run.subpart), 1};
    const auto dentry = plan_directory_dentry_insert(parent, name, inode_location);
    if (!dentry.ok) {
        result.error = dentry.error;
        return result;
    }

    FileWriteResult stats;
    std::vector<BitmapKey> reserved_dirty;
    std::string bitmap_error;
    for (std::uint32_t i = 0; i < 2; ++i) {
        if (!set_zone(run.subpart, run.first + i, true, reserved_dirty, bitmap_error)) {
            result.error = bitmap_error;
            discard_bitmap_cache(reserved_dirty);
            return result;
        }
    }
    if (!commit_bitmap_changes(reserved_dirty, stats, "reserve PFS directory inode/data zones")) {
        result.error = stats.error;
        return result;
    }
    result.metadata_transactions += stats.metadata_transactions;
    result.bitmap_chunks_touched += stats.bitmap_chunks_touched;

    const auto cleanup_reserved = [&]() {
        FileWriteResult cleanup_stats;
        std::vector<BitmapKey> dirty;
        std::string cleanup_error;
        for (std::uint32_t i = 0; i < 2; ++i) {
            if (!set_zone(run.subpart, run.first + i, false, dirty, cleanup_error)) {
                result.warning = "Could not release reserved PFS directory zones: " + cleanup_error;
                return;
            }
        }
        if (!commit_bitmap_changes(dirty, cleanup_stats, "release abandoned PFS directory zones")) {
            result.warning = "PFS mkdir failed safely, but reserved zones remain allocated: " +
                             cleanup_stats.error;
            return;
        }
        result.metadata_transactions += cleanup_stats.metadata_transactions;
        result.bitmap_chunks_touched += cleanup_stats.bitmap_chunks_touched;
    };

    std::array<std::byte, apa::kSectorSize> directory_sector{};
    put_dentry(directory_sector, 0, inode_location, ".", kModeDirectory, 12);
    put_dentry(directory_sector, 12, parent.location, "..", kModeDirectory, 500);

    const auto data_sector64 = static_cast<std::uint64_t>(run.first + 1U) * sectors_per_zone();
    std::uint64_t data_offset = 0;
    if (data_sector64 > std::numeric_limits<std::uint32_t>::max() ||
        !writable_volume_.absolute_byte_offset(run.subpart,
                                               static_cast<std::uint32_t>(data_sector64),
                                               1, data_offset)) {
        result.error = "PFS directory data sector maps outside its APA extent";
        cleanup_reserved();
        return result;
    }

    {
        WriteTransaction transaction(device_);
        if (!transaction.stage(data_offset, directory_sector, "initialize PFS directory data")) {
            result.error = "Could not stage PFS directory data: " + transaction.stage_error();
            cleanup_reserved();
            return result;
        }
        const auto committed = transaction.commit();
        if (!committed.ok) {
            result.error = "PFS directory data transaction failed: " + committed.error;
            cleanup_reserved();
            return result;
        }
        ++result.metadata_transactions;
    }

    Inode inode{};
    inode.magic = kSegdMagic;
    inode.inode_block = inode_location;
    inode.last_segment = inode_location;
    inode.data[0] = inode_location;
    inode.data[1] = {run.first + 1U, static_cast<std::uint16_t>(run.subpart), 1};
    inode.mode = options.mode;
    inode.attr = 0x00A0U;
    inode.uid = options.uid;
    inode.gid = options.gid;
    inode.atime = options.timestamp;
    inode.ctime = options.timestamp;
    inode.mtime = options.timestamp;
    inode.size = apa::kSectorSize;
    inode.number_blocks = 2;
    inode.number_data = 2;
    inode.number_segdesg = 1;
    inode.subpart = static_cast<std::uint32_t>(run.subpart);
    inode.checksum = inode_checksum(inode);

    const auto inode_block64 = static_cast<std::uint64_t>(inode.inode_block.number) << inode_scale();
    const auto inode_sector64 = inode_block64 * kMetadataSectors;
    std::uint64_t inode_offset = 0;
    std::uint64_t dentry_offset = 0;
    if (inode_sector64 > std::numeric_limits<std::uint32_t>::max() ||
        !writable_volume_.absolute_byte_offset(inode.inode_block.subpart,
                                               static_cast<std::uint32_t>(inode_sector64),
                                               kMetadataSectors, inode_offset) ||
        !writable_volume_.absolute_byte_offset(dentry.subpart, dentry.sector, 1, dentry_offset)) {
        result.error = "PFS mkdir publication maps outside its APA extent";
        cleanup_reserved();
        return result;
    }

    WriteTransaction publication(device_);
    if (!publication.stage(inode_offset, std::as_bytes(std::span{&inode, 1}),
                           "publish PFS directory inode") ||
        !publication.stage(dentry_offset, dentry.sector_bytes,
                           "publish PFS parent directory entry")) {
        result.error = "Could not stage PFS mkdir publication: " + publication.stage_error();
        cleanup_reserved();
        return result;
    }

    const auto committed = publication.commit([&]() -> std::string {
        Reader reader(read_volume_);
        if (!reader.valid()) {
            return "PFS probe failed after mkdir publication";
        }
        const auto node = reader.resolve(full_path);
        if (!node || (node->inode.mode & kModeMask) != kModeDirectory) {
            return "Created PFS directory does not resolve as a directory";
        }
        const auto entries = reader.list_directory(*node, true);
        const auto self = std::find_if(entries.begin(), entries.end(), [](const DirectoryEntry& entry) {
            return entry.name == ".";
        });
        const auto up = std::find_if(entries.begin(), entries.end(), [](const DirectoryEntry& entry) {
            return entry.name == "..";
        });
        if (self == entries.end() || up == entries.end() ||
            !same_block(self->inode, inode_location) || !same_block(up->inode, parent.location)) {
            return "Created PFS directory dot entries failed verification";
        }
        return {};
    });
    if (!committed.ok) {
        result.error = "PFS mkdir publication failed: " + committed.error;
        cleanup_reserved();
        return result;
    }

    ++result.metadata_transactions;
    result.created_components = 1;
    result.ok = true;
    return result;
}

DirectoryEnsureResult ImageWriter::ensure_directory(std::string_view input_path,
                                                     const DirectoryWriteOptions& options)
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

    const std::string path = normalize_path(input_path);
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
        if (!reader.valid()) {
            result.error = "PFS changed or became invalid during mkdir session";
            return result;
        }
        const auto parent = reader.resolve(parent_path);
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

        const std::string full_path = parent_path == "/"
                                          ? "/" + component
                                          : parent_path + "/" + component;
        if (existing != entries.end()) {
            const auto node = reader.read_inode(existing->inode);
            if (!node || (node->inode.mode & kModeMask) != kModeDirectory) {
                result.error = "PFS mkdir path component already exists and is not a directory: " +
                               full_path;
                return result;
            }
        } else {
            const auto created = create_directory(*parent, full_path, component, options);
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

} // namespace ps2hdd::pfs
