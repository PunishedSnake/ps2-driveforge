#include "ps2hdd/mount_view.hpp"

#include "ps2hdd/pfs_export.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <utility>

namespace ps2hdd {
namespace {

struct AliasEntry {
    std::string visible;
    std::string backing;
    MountNodeKind kind{MountNodeKind::pfs_file};
    std::uint64_t size{};
};

std::string uppercase_ascii(std::string_view value)
{
    std::string result(value);
    for (char& c : result) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            c = static_cast<char>(std::toupper(byte));
        }
    }
    return result;
}

bool equal_windows_name(std::string_view left, std::string_view right)
{
    return uppercase_ascii(left) == uppercase_ascii(right);
}

std::vector<std::string> split_path(std::string_view path)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < path.size()) {
        while (start < path.size() && (path[start] == '/' || path[start] == '\\')) {
            ++start;
        }
        if (start == path.size()) {
            break;
        }
        auto end = start;
        while (end < path.size() && path[end] != '/' && path[end] != '\\') {
            ++end;
        }
        parts.emplace_back(path.substr(start, end - start));
        start = end;
    }
    return parts;
}

std::string join_pfs_path(std::string_view parent, std::string_view child)
{
    if (parent.empty() || parent == "/") {
        return std::string(child);
    }
    std::string result(parent);
    result.push_back('/');
    result.append(child);
    return result;
}

std::vector<AliasEntry> make_entry_aliases(const std::vector<SessionEntry>& entries)
{
    std::vector<AliasEntry> aliases;
    aliases.reserve(entries.size());
    std::set<std::string> used;

    for (const auto& entry : entries) {
        if (!entry.inode_readable || (!entry.is_directory() && !entry.is_regular())) {
            continue;
        }
        std::string visible = pfs::sanitize_host_filename(entry.name);
        const auto base = visible;
        unsigned suffix = 2;
        while (!used.insert(uppercase_ascii(visible)).second) {
            visible = base + "_" + std::to_string(suffix++);
        }
        aliases.push_back({
            std::move(visible),
            entry.name,
            entry.is_directory() ? MountNodeKind::pfs_directory : MountNodeKind::pfs_file,
            entry.size,
        });
    }
    return aliases;
}

std::vector<AliasEntry> make_partition_aliases(const apa::ScanResult& scan)
{
    std::vector<AliasEntry> aliases;
    std::set<std::string> used;
    for (const auto& partition : scan.partitions) {
        if (partition.is_sub() || partition.type != apa::kTypePfs) {
            continue;
        }
        std::string visible = pfs::sanitize_host_filename(partition.id);
        const auto base = visible;
        unsigned suffix = 2;
        while (!used.insert(uppercase_ascii(visible)).second) {
            visible = base + "_" + std::to_string(suffix++);
        }
        aliases.push_back({std::move(visible), partition.id, MountNodeKind::pfs_directory, 0});
    }
    return aliases;
}

const AliasEntry* find_alias(const std::vector<AliasEntry>& aliases, std::string_view visible)
{
    const auto it = std::find_if(aliases.begin(), aliases.end(), [visible](const AliasEntry& entry) {
        return equal_windows_name(entry.visible, visible);
    });
    return it == aliases.end() ? nullptr : &*it;
}

bool forbidden_component(std::string_view component)
{
    return component == "." || component == "..";
}

} // namespace

MountLookupResult ReadOnlyMountView::lookup(std::string_view path)
{
    MountLookupResult result;
    const auto parts = split_path(path);
    if (parts.empty()) {
        result.ok = true;
        result.kind = MountNodeKind::root;
        return result;
    }
    for (const auto& part : parts) {
        if (forbidden_component(part)) {
            result.error = "Dot path components are not valid in the mounted namespace";
            return result;
        }
    }

    if (!equal_windows_name(parts[0], "Partitions")) {
        result.error = "Mounted path does not exist";
        return result;
    }
    if (parts.size() == 1) {
        result.ok = true;
        result.kind = MountNodeKind::namespace_directory;
        return result;
    }

    const auto partitions = make_partition_aliases(session_.scan_result());
    const auto* partition = find_alias(partitions, parts[1]);
    if (!partition) {
        result.error = "PFS partition does not exist";
        return result;
    }
    result.partition = partition->backing;
    result.kind = MountNodeKind::pfs_directory;
    if (parts.size() == 2) {
        result.ok = true;
        return result;
    }

    std::string backing_path;
    for (std::size_t index = 2; index < parts.size(); ++index) {
        const auto browse = session_.browse(result.partition, backing_path);
        if (!browse.ok) {
            result.error = browse.error;
            return result;
        }
        const auto aliases = make_entry_aliases(browse.entries);
        const auto* entry = find_alias(aliases, parts[index]);
        if (!entry) {
            result.error = "PFS path does not exist";
            return result;
        }

        backing_path = join_pfs_path(backing_path, entry->backing);
        const bool last = index + 1 == parts.size();
        if (!last && entry->kind != MountNodeKind::pfs_directory) {
            result.error = "Mounted path traverses through a regular file";
            return result;
        }
        if (last) {
            result.kind = entry->kind;
            result.size = entry->size;
        }
    }

    result.pfs_path = std::move(backing_path);
    result.ok = true;
    return result;
}

MountListResult ReadOnlyMountView::list_directory(std::string_view path)
{
    MountListResult result;
    const auto node = lookup(path);
    if (!node.ok) {
        result.error = node.error;
        return result;
    }
    if (!node.is_directory()) {
        result.error = "Mounted path is not a directory";
        return result;
    }

    if (node.kind == MountNodeKind::root) {
        result.entries.push_back({"Partitions", MountNodeKind::namespace_directory, 0});
        result.ok = true;
        return result;
    }
    if (node.kind == MountNodeKind::namespace_directory) {
        const auto aliases = make_partition_aliases(session_.scan_result());
        result.entries.reserve(aliases.size());
        for (const auto& alias : aliases) {
            result.entries.push_back({alias.visible, alias.kind, 0});
        }
        result.ok = true;
        return result;
    }

    const auto browse = session_.browse(node.partition, node.pfs_path);
    if (!browse.ok) {
        result.error = browse.error;
        return result;
    }
    const auto aliases = make_entry_aliases(browse.entries);
    result.entries.reserve(aliases.size());
    for (const auto& alias : aliases) {
        result.entries.push_back({alias.visible, alias.kind, alias.size});
    }
    result.ok = true;
    return result;
}

MountReadResult ReadOnlyMountView::read_file(std::string_view path, std::uint64_t offset,
                                             std::span<std::byte> out)
{
    MountReadResult result;
    const auto node = lookup(path);
    if (!node.ok) {
        result.error = node.error;
        return result;
    }
    if (node.kind != MountNodeKind::pfs_file) {
        result.error = "Mounted path is not a regular file";
        return result;
    }

    const auto read = session_.read_file(node.partition, node.pfs_path, offset, out);
    result.ok = read.ok;
    result.error = read.error;
    result.bytes_read = read.bytes_read;
    return result;
}

} // namespace ps2hdd
