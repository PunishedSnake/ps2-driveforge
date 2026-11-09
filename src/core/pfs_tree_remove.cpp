#include "ps2hdd/pfs_write.hpp"

#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace ps2hdd::pfs {
namespace {

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

std::uint64_t zone_count(std::span<const ImageWriter::ZoneRun> runs) noexcept
{
    std::uint64_t total = 0;
    for (const auto& run : runs) {
        total += run.count;
    }
    return total;
}

bool safe_child_name(std::string_view name) noexcept
{
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos;
}

} // namespace

DirectoryRemoveResult ImageWriter::remove_empty_directory(std::string_view input_path)
{
    DirectoryRemoveResult result;
    if (!valid()) {
        result.error = error_.empty() ? "PFS writer session is invalid" : error_;
        return result;
    }

    const auto path = normalize_path(input_path);
    result.path = path;
    if (path.empty() || path == "/" || contains_parent_escape(path)) {
        result.error = "PFS directory unlink requires a normal non-root path";
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
        result.error = "PFS directory unlink parent was not found";
        return result;
    }
    const auto node = reader.resolve(path);
    if (!node || (node->inode.mode & kModeMask) != kModeDirectory) {
        result.error = "PFS directory unlink target is not a directory";
        return result;
    }
    const auto children = reader.list_directory(*node, false);
    if (!reader.last_error().empty()) {
        result.error = "Could not enumerate PFS directory before unlink: " + reader.last_error();
        return result;
    }
    if (!children.empty()) {
        result.error = "PFS directory unlink requires an empty directory";
        return result;
    }

    const auto map = reader.extent_map(*node);
    if (!map) {
        result.error = "Could not inspect PFS directory extent graph: " + reader.last_error();
        return result;
    }

    std::vector<ZoneRun> release;
    release.push_back({node->location.subpart, node->location.number, 1});
    for (const auto& block : map->data) {
        release.push_back({block.subpart, block.number, block.count});
    }
    for (const auto& block : map->indirect_descriptors) {
        release.push_back({block.subpart, block.number, 1});
    }

    std::unordered_set<std::uint64_t> seen;
    for (const auto& run : release) {
        for (std::uint32_t index = 0; index < run.count; ++index) {
            const auto key = (static_cast<std::uint64_t>(run.subpart) << 32U) |
                             (run.first + index);
            if (!seen.insert(key).second || !zone_used(run.subpart, run.first + index) ||
                !error_.empty()) {
                result.error = "PFS directory allocation graph is duplicated, invalid or bitmap-free";
                return result;
            }
        }
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
        result.error = "PFS directory dentry maps outside APA extent";
        return result;
    }

    WriteTransaction unlink(device_);
    if (!unlink.stage(dentry_offset, dentry.sector_bytes, "unlink empty PFS directory")) {
        result.error = "Could not stage PFS directory unlink: " + unlink.stage_error();
        return result;
    }
    const auto committed = unlink.commit([&]() -> std::string {
        Reader verify(read_volume_, probe_);
        if (verify.resolve(path)) {
            return "PFS directory still resolves after unlink";
        }
        const auto verify_parent = verify.resolve(parent_path);
        if (!verify_parent) {
            return "PFS parent became unreadable after directory unlink";
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
    if (!release_runs(release, release_stats, "release unlinked PFS directory allocation")) {
        result.warning = "Directory is unlinked but its PFS allocation remains reserved: " +
                         release_stats.error;
        result.ok = true;
        return result;
    }
    result.metadata_transactions += release_stats.metadata_transactions;
    result.bitmap_chunks_touched += release_stats.bitmap_chunks_touched;
    result.bytes_freed = zone_count(release) * probe_.super.zone_size;
    result.ok = true;
    return result;
}

TreeRemoveResult ImageWriter::remove_tree(std::string_view input_path)
{
    TreeRemoveResult result;
    if (!valid()) {
        result.error = error_.empty() ? "PFS writer session is invalid" : error_;
        return result;
    }

    const auto root_path = normalize_path(input_path);
    result.path = root_path;
    if (root_path.empty() || root_path == "/" || contains_parent_escape(root_path)) {
        result.error = "PFS recursive removal requires a normal non-root path";
        return result;
    }

    constexpr std::size_t kMaxDepth = 128;
    constexpr std::size_t kMaxNodes = 100000;
    std::size_t visited = 0;

    auto append_warning = [&](const std::string& warning) {
        if (warning.empty()) {
            return;
        }
        if (!result.warning.empty()) {
            result.warning += "; ";
        }
        result.warning += warning;
    };

    std::function<bool(const std::string&, std::size_t)> remove_node;
    remove_node = [&](const std::string& path, std::size_t depth) -> bool {
        if (depth > kMaxDepth || ++visited > kMaxNodes) {
            result.error = "PFS recursive removal exceeded traversal safety limits";
            return false;
        }

        Reader reader(read_volume_, probe_);
        const auto node = reader.resolve(path);
        if (!node) {
            result.error = "PFS recursive removal could not resolve " + path + ": " + reader.last_error();
            return false;
        }

        const auto mode = static_cast<std::uint16_t>(node->inode.mode & kModeMask);
        if (mode == kModeRegular) {
            const auto removed = remove_file_full(path);
            if (!removed.ok) {
                result.error = removed.error;
                append_warning(removed.warning);
                return false;
            }
            ++result.files_removed;
            result.bytes_freed += removed.bytes_freed;
            result.metadata_transactions += removed.metadata_transactions;
            result.bitmap_chunks_touched += removed.bitmap_chunks_touched;
            append_warning(removed.warning);
            return true;
        }
        if (mode != kModeDirectory) {
            result.error = "PFS recursive removal encountered an unsupported inode type at " + path;
            return false;
        }

        const auto entries = reader.list_directory(*node, false);
        if (!reader.last_error().empty()) {
            result.error = "Could not enumerate PFS directory during recursive removal: " +
                           reader.last_error();
            return false;
        }
        for (const auto& entry : entries) {
            if (!safe_child_name(entry.name)) {
                result.error = "PFS recursive removal encountered an unsafe directory entry name";
                return false;
            }
            const auto child = path + "/" + entry.name;
            if (!remove_node(child, depth + 1U)) {
                return false;
            }
        }

        const auto removed = remove_empty_directory(path);
        if (!removed.ok) {
            result.error = removed.error;
            append_warning(removed.warning);
            return false;
        }
        ++result.directories_removed;
        result.bytes_freed += removed.bytes_freed;
        result.metadata_transactions += removed.metadata_transactions;
        result.bitmap_chunks_touched += removed.bitmap_chunks_touched;
        append_warning(removed.warning);
        return true;
    };

    if (!remove_node(root_path, 0)) {
        result.partial = result.files_removed != 0 || result.directories_removed != 0;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::pfs
