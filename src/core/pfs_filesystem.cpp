#include "ps2hdd/pfs_filesystem.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>

namespace ps2hdd::pfs {

static_assert(std::endian::native == std::endian::little,
              "PFS reader currently requires a little-endian host");

namespace {

void set_error(std::string* error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool block_info_empty(const BlockInfo& bi) noexcept
{
    return bi.number == 0 && bi.subpart == 0 && bi.count == 0;
}

std::uint16_t entry_type(std::uint16_t allocated_length) noexcept
{
    return allocated_length & kModeTypeMask;
}

std::uint16_t entry_length(std::uint16_t allocated_length) noexcept
{
    return allocated_length & 0x0FFFU;
}

} // namespace

std::uint32_t inode_checksum(const Inode& inode) noexcept
{
    std::array<std::uint32_t, kMetaSize / sizeof(std::uint32_t)> words{};
    std::memcpy(words.data(), &inode, sizeof(inode));
    std::uint32_t sum = 0;
    for (std::size_t i = 1; i < words.size(); ++i) {
        sum += words[i];
    }
    return sum;
}

bool FileSystem::mount()
{
    clear_metadata_cache();
    probe_ = probe(volume_);
    mounted_ = false;
    zone_sectors_ = 0;

    if (!probe_.valid) {
        return false;
    }
    if ((probe_.super.zone_size % apa::kSectorSize) != 0) {
        probe_.errors.emplace_back("PFS zone size is not sector aligned");
        return false;
    }

    zone_sectors_ = probe_.super.zone_size / static_cast<std::uint32_t>(apa::kSectorSize);
    if (zone_sectors_ == 0) {
        probe_.errors.emplace_back("PFS zone size produced a zero sector scale");
        return false;
    }

    Inode root{};
    std::string error;
    if (!read_inode(probe_.super.root, root, &error)) {
        probe_.errors.emplace_back("Could not read root inode: " + error);
        return false;
    }
    if ((root.mode & kModeTypeMask) != kModeDirectory) {
        probe_.errors.emplace_back("PFS root inode is not a directory");
        return false;
    }

    mounted_ = true;
    return true;
}

bool FileSystem::read_raw_inode(const BlockInfo& location, std::uint32_t expected_magic,
                                RawInode& out, std::string* error)
{
    if (!probe_.valid || zone_sectors_ == 0) {
        set_error(error, "PFS is not mounted/probed");
        return false;
    }
    if (location.subpart >= volume_.extent_count()) {
        set_error(error, "inode points to a missing PFS sub-partition");
        return false;
    }

    const std::uint64_t sector64 = static_cast<std::uint64_t>(location.number) * zone_sectors_;
    if (sector64 > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "inode sector exceeds the current 32-bit PFS address space");
        return false;
    }

    if (!volume_.read_sectors(location.subpart, static_cast<std::uint32_t>(sector64), 2,
                              out.bytes)) {
        set_error(error, "could not read 1024-byte inode metadata block");
        return false;
    }

    std::memcpy(&out.parsed, out.bytes.data(), sizeof(out.parsed));
    if (out.parsed.magic != expected_magic) {
        set_error(error, expected_magic == kSegdMagic ? "inode SEGD magic is invalid"
                                                      : "indirect inode SEGI magic is invalid");
        return false;
    }
    if (inode_checksum(out.parsed) != out.parsed.checksum) {
        set_error(error, "inode checksum is invalid");
        return false;
    }
    return true;
}

void FileSystem::clear_metadata_cache()
{
    std::unique_lock lock(cache_mutex_);
    inode_cache_.clear();
    directory_cache_.clear();
}

bool FileSystem::read_inode(const BlockInfo& location, Inode& out, std::string* error)
{
    const auto key = cache_key(location);
    {
        std::shared_lock lock(cache_mutex_);
        const auto it = inode_cache_.find(key);
        if (it != inode_cache_.end()) {
            out = it->second;
            return true;
        }
    }

    RawInode raw{};
    if (!read_raw_inode(location, kSegdMagic, raw, error)) {
        return false;
    }
    out = raw.parsed;
    {
        std::unique_lock lock(cache_mutex_);
        inode_cache_.insert_or_assign(key, out);
    }
    return true;
}

bool FileSystem::collect_data_extents(const Inode& inode, std::vector<BlockInfo>& extents,
                                      std::string* error)
{
    extents.clear();

    if (inode.number_data == 0) {
        if (inode.size != 0) {
            set_error(error, "inode has data but number_data is zero");
            return false;
        }
        return true;
    }

    // Global segment 0 is the primary SEGD itself. Direct file data occupies 1..113.
    const std::uint32_t direct_end = std::min<std::uint32_t>(inode.number_data,
                                                             kDirectBlockInfos);
    for (std::uint32_t index = 1; index < direct_end; ++index) {
        const auto& bi = inode.data[index];
        if (bi.count == 0) {
            set_error(error, "inode contains a zero-length direct data extent");
            return false;
        }
        if (bi.subpart >= volume_.extent_count()) {
            set_error(error, "inode data extent points to a missing sub-partition");
            return false;
        }
        extents.push_back(bi);
    }

    if (inode.number_data <= kDirectBlockInfos) {
        return true;
    }

    BlockInfo next = inode.next_segment;
    std::uint32_t global_index = static_cast<std::uint32_t>(kDirectBlockInfos);
    std::size_t chain_guard = 0;

    // PS2SDK stores 123 BlockInfo records in an indirect SEGI descriptor. The records occupy
    // all bytes after the fixed 40-byte inode prefix; unlike a primary SEGD, there is no
    // file metadata tail to preserve there. Local entry 0 is the SEGI descriptor itself.
    while (global_index < inode.number_data) {
        if (block_info_empty(next)) {
            set_error(error, "inode requires an indirect segment descriptor but next_segment is empty");
            return false;
        }
        if (++chain_guard > 4096) {
            set_error(error, "indirect inode chain is implausibly long");
            return false;
        }

        RawInode indirect{};
        if (!read_raw_inode(next, kSegiMagic, indirect, error)) {
            return false;
        }

        constexpr std::size_t data_offset = offsetof(Inode, data);
        for (std::size_t local_index = 1;
             local_index < kIndirectBlockInfos && global_index + local_index < inode.number_data;
             ++local_index) {
            BlockInfo bi{};
            std::memcpy(&bi,
                        indirect.bytes.data() + data_offset + local_index * sizeof(BlockInfo),
                        sizeof(bi));
            if (bi.count == 0) {
                set_error(error, "indirect inode contains a zero-length data extent");
                return false;
            }
            if (bi.subpart >= volume_.extent_count()) {
                set_error(error, "indirect data extent points to a missing sub-partition");
                return false;
            }
            extents.push_back(bi);
        }

        global_index += static_cast<std::uint32_t>(kIndirectBlockInfos);
        next = indirect.parsed.next_segment;
    }

    return true;
}

bool FileSystem::read_volume_bytes(std::uint16_t subpart, std::uint64_t byte_offset,
                                   std::span<std::byte> out, std::string* error)
{
    if (out.empty()) {
        return true;
    }
    if (subpart >= volume_.extent_count()) {
        set_error(error, "read points to a missing sub-partition");
        return false;
    }

    std::size_t done = 0;
    while (done < out.size()) {
        const std::uint64_t absolute = byte_offset + done;
        const std::uint64_t sector64 = absolute / apa::kSectorSize;
        const std::size_t in_sector = static_cast<std::size_t>(absolute % apa::kSectorSize);
        if (sector64 > std::numeric_limits<std::uint32_t>::max()) {
            set_error(error, "PFS byte offset exceeds the current 32-bit sector address space");
            return false;
        }

        if (in_sector == 0 && out.size() - done >= apa::kSectorSize) {
            const std::size_t full_sectors = (out.size() - done) / apa::kSectorSize;
            const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(full_sectors, 2048));
            const std::size_t bytes = static_cast<std::size_t>(count) * apa::kSectorSize;
            if (!volume_.read_sectors(subpart, static_cast<std::uint32_t>(sector64), count,
                                      out.subspan(done, bytes))) {
                set_error(error, "could not read PFS data sectors");
                return false;
            }
            done += bytes;
            continue;
        }

        std::array<std::byte, apa::kSectorSize> sector{};
        if (!volume_.read_sectors(subpart, static_cast<std::uint32_t>(sector64), 1, sector)) {
            set_error(error, "could not read a partial PFS data sector");
            return false;
        }
        const std::size_t bytes = std::min(out.size() - done, apa::kSectorSize - in_sector);
        std::memcpy(out.data() + done, sector.data() + in_sector, bytes);
        done += bytes;
    }
    return true;
}

bool FileSystem::read_inode_data(const Inode& inode, std::uint64_t offset,
                                 std::span<std::byte> out, std::size_t& bytes_read,
                                 std::string* error)
{
    bytes_read = 0;
    if (offset >= inode.size || out.empty()) {
        return true;
    }

    const std::uint64_t wanted64 = std::min<std::uint64_t>(out.size(), inode.size - offset);
    const std::size_t wanted = static_cast<std::size_t>(wanted64);

    std::vector<BlockInfo> extents;
    if (!collect_data_extents(inode, extents, error)) {
        return false;
    }

    std::uint64_t logical_base = 0;
    for (const auto& extent : extents) {
        const std::uint64_t extent_bytes = static_cast<std::uint64_t>(extent.count) * probe_.super.zone_size;
        if (offset >= logical_base + extent_bytes) {
            logical_base += extent_bytes;
            continue;
        }

        const std::uint64_t within = offset > logical_base ? offset - logical_base : 0;
        const std::uint64_t available = extent_bytes - within;
        const std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>(available, wanted - bytes_read));
        const std::uint64_t physical_byte =
            static_cast<std::uint64_t>(extent.number) * probe_.super.zone_size + within;

        if (!read_volume_bytes(extent.subpart, physical_byte,
                               out.subspan(bytes_read, take), error)) {
            return false;
        }

        bytes_read += take;
        offset += take;
        if (bytes_read == wanted) {
            return true;
        }
        logical_base += extent_bytes;
    }

    if (bytes_read != wanted) {
        set_error(error, "inode extents are shorter than the file size");
        return false;
    }
    return true;
}

DirectoryResult FileSystem::list_directory(const BlockInfo& location, bool include_special)
{
    DirectoryResult result;
    if (!mounted_) {
        result.errors.emplace_back("PFS is not mounted");
        return result;
    }

    // Keep two cache namespaces: visible entries and visible+special entries.
    const auto base_key = cache_key(location);
    const auto directory_key = base_key ^ (include_special ? (1ULL << 63U) : 0ULL);
    {
        std::shared_lock lock(cache_mutex_);
        const auto it = directory_cache_.find(directory_key);
        if (it != directory_cache_.end()) {
            return it->second;
        }
    }

    Inode inode{};
    std::string error;
    if (!read_inode(location, inode, &error)) {
        result.errors.emplace_back(error);
        return result;
    }
    if ((inode.mode & kModeTypeMask) != kModeDirectory) {
        result.errors.emplace_back("requested inode is not a directory");
        return result;
    }

    std::uint64_t position = 0;
    while (position < inode.size) {
        std::array<std::byte, apa::kSectorSize> sector{};
        std::size_t bytes_read = 0;
        if (!read_inode_data(inode, position, sector, bytes_read, &error)) {
            result.errors.emplace_back(error);
            return result;
        }
        if (bytes_read == 0) {
            result.errors.emplace_back("directory data ended before inode size");
            return result;
        }

        std::size_t offset = 0;
        while (offset + sizeof(DirectoryEntryHeader) <= bytes_read) {
            DirectoryEntryHeader header{};
            std::memcpy(&header, sector.data() + offset, sizeof(header));
            const std::uint16_t allocated = entry_length(header.allocated_length);

            if (allocated == 0) {
                result.errors.emplace_back("directory entry has zero allocated length");
                return result;
            }
            if ((allocated & 3U) != 0) {
                result.errors.emplace_back("directory entry length is not 4-byte aligned");
                return result;
            }
            if (allocated < sizeof(DirectoryEntryHeader) ||
                offset + allocated > apa::kSectorSize ||
                offset + allocated > bytes_read) {
                result.errors.emplace_back("directory entry exceeds the available 512-byte sector data");
                return result;
            }
            if (header.path_length > allocated - sizeof(DirectoryEntryHeader)) {
                result.errors.emplace_back("directory entry path length exceeds its allocation");
                return result;
            }

            if (header.inode != 0 && header.path_length != 0) {
                const auto* name_ptr = reinterpret_cast<const char*>(sector.data() + offset + sizeof(header));
                std::string name(name_ptr, name_ptr + header.path_length);
                if (include_special || (name != "." && name != "..")) {
                    DirectoryEntry entry;
                    entry.name = std::move(name);
                    entry.location.number = header.inode;
                    entry.location.subpart = header.sub;
                    entry.location.count = 1;
                    entry.type = entry_type(header.allocated_length);
                    result.entries.push_back(std::move(entry));
                }
            }

            offset += allocated;
            if (offset == apa::kSectorSize) {
                break;
            }
        }

        position += bytes_read;
    }

    if (result.ok()) {
        std::unique_lock lock(cache_mutex_);
        directory_cache_.insert_or_assign(directory_key, result);
    }
    return result;
}

std::optional<Node> FileSystem::resolve(std::string_view path, std::string* error)
{
    if (!mounted_) {
        set_error(error, "PFS is not mounted");
        return std::nullopt;
    }

    BlockInfo current = probe_.super.root;
    std::size_t begin = 0;

    while (begin < path.size() && (path[begin] == '/' || path[begin] == '\\')) {
        ++begin;
    }

    while (begin < path.size()) {
        std::size_t end = begin;
        while (end < path.size() && path[end] != '/' && path[end] != '\\') {
            ++end;
        }
        const std::string_view component = path.substr(begin, end - begin);
        if (!component.empty() && component != ".") {
            const auto listing = list_directory(current, component == "..");
            if (!listing.ok()) {
                set_error(error, listing.errors.front());
                return std::nullopt;
            }
            const auto it = std::find_if(listing.entries.begin(), listing.entries.end(),
                                         [&](const DirectoryEntry& entry) {
                                             return entry.name == component;
                                         });
            if (it == listing.entries.end()) {
                set_error(error, "PFS path component not found: " + std::string(component));
                return std::nullopt;
            }
            current = it->location;
        }
        begin = end;
        while (begin < path.size() && (path[begin] == '/' || path[begin] == '\\')) {
            ++begin;
        }
    }

    Node node;
    node.location = current;
    if (!read_inode(current, node.inode, error)) {
        return std::nullopt;
    }
    return node;
}

bool FileSystem::read_file(const Node& node, std::uint64_t offset, std::span<std::byte> out,
                           std::size_t& bytes_read, std::string* error)
{
    if (!mounted_) {
        set_error(error, "PFS is not mounted");
        bytes_read = 0;
        return false;
    }
    if (node.is_directory()) {
        set_error(error, "cannot read a directory through read_file");
        bytes_read = 0;
        return false;
    }
    return read_inode_data(node.inode, offset, out, bytes_read, error);
}

} // namespace ps2hdd::pfs
