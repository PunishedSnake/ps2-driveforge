#include "ps2hdd/writable_mount_view.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace ps2hdd {
namespace {

std::string upper(std::string_view value)
{
    std::string out(value);
    for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return out;
}

std::pair<std::string, std::string> split_parent(std::string_view path)
{
    std::string p(path);
    while (p.size() > 1 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    const auto slash = p.find_last_of("\\/");
    if (slash == std::string::npos) return {"\\", p};
    return {slash == 0 ? "\\" : p.substr(0, slash), p.substr(slash + 1)};
}

std::string pfs_child(const std::string& parent, const std::string& leaf)
{
    return parent == "/" ? "/" + leaf : parent + "/" + leaf;
}

} // namespace

WritableMountView::Target WritableMountView::resolve(std::string_view path)
{
    Target target;
    const auto base = base_.lookup(path);
    if (base.ok) {
        if (base.kind != MountNodeKind::pfs_directory && base.kind != MountNodeKind::pfs_file) {
            target.ok = true; // namespace node: exists, never a PFS target
            target.exists = true;
            target.is_directory = base.is_directory();
            return target;
        }
        target.ok = true;
        target.partition = base.partition;
        target.pfs_path = base.pfs_path.empty() ? "/" : base.pfs_path;
        target.exists = true;
        target.is_directory = base.is_directory();
        target.size = base.size;
    } else {
        const auto [parent_path, leaf] = split_parent(path);
        if (leaf.empty() || parent_path == path) {
            target.error = base.error;
            return target;
        }
        const auto parent = resolve(parent_path);
        if (!parent.ok || parent.partition.empty() || !parent.exists || !parent.is_directory) {
            target.error = parent.ok ? "Parent is not a PFS directory" : parent.error;
            return target;
        }
        target.ok = true;
        target.partition = parent.partition;
        target.pfs_path = pfs_child(parent.pfs_path, leaf);
    }

    if (const auto pending = engine_.pending(target.partition, target.pfs_path)) {
        target.exists = pending->kind != PendingNode::Kind::deleted;
        target.is_directory = pending->kind == PendingNode::Kind::directory;
        target.size = pending->size;
        if (pending->kind == PendingNode::Kind::file) target.pfs_path = pending->path;
    }
    target.writable = engine_.partition_writable(target.partition) && !engine_.read_only();
    return target;
}

MountLookupResult WritableMountView::lookup(std::string_view path)
{
    const auto target = resolve(path);
    if (!target.ok || target.partition.empty()) return base_.lookup(path);
    MountLookupResult result;
    if (!target.exists) {
        result.error = "Mounted path does not exist";
        return result;
    }
    result.ok = true;
    result.kind = target.is_directory ? MountNodeKind::pfs_directory : MountNodeKind::pfs_file;
    result.partition = target.partition;
    result.pfs_path = target.pfs_path;
    result.size = target.size;
    return result;
}

MountListResult WritableMountView::list_directory(std::string_view path)
{
    const auto target = resolve(path);
    if (!target.ok || target.partition.empty()) return base_.list_directory(path);
    MountListResult result;
    if (!target.exists || !target.is_directory) {
        result.error = target.exists ? "Mounted path is not a directory" : "Mounted path does not exist";
        return result;
    }
    // Base listing (disk, with Windows-safe aliases) for directories that are
    // on disk; merged children from the engine for the overlay.
    std::map<std::string, MountEntry> merged;
    const auto base = base_.list_directory(path);
    if (base.ok) {
        for (const auto& entry : base.entries) merged[upper(entry.name)] = entry;
    }
    std::map<std::string, PendingNode> overlay;
    for (auto& node : engine_.pending_children(target.partition, target.pfs_path)) {
        const auto slash = node.path.rfind('/');
        overlay[upper(node.path.substr(slash + 1))] = std::move(node);
    }
    for (auto it = merged.begin(); it != merged.end();) {
        if (it->second.is_directory() || it->second.kind == MountNodeKind::pfs_file) {
            const auto found = overlay.find(it->first);
            if (found == overlay.end()) {
                // Hidden only if the engine says this exact name is deleted;
                // aliased legacy names never match and stay visible.
                const auto pending = engine_.pending(target.partition, pfs_child(target.pfs_path, it->second.name));
                if (pending && pending->kind == PendingNode::Kind::deleted) {
                    it = merged.erase(it);
                    continue;
                }
            } else if (found->second.kind == PendingNode::Kind::file) {
                it->second.size = found->second.size;
            }
        }
        ++it;
    }
    for (const auto& [key, node] : overlay) {
        if (merged.contains(key)) continue;
        MountEntry entry;
        entry.name = node.path.substr(node.path.rfind('/') + 1);
        entry.kind = node.kind == PendingNode::Kind::directory ? MountNodeKind::pfs_directory
                                                               : MountNodeKind::pfs_file;
        entry.size = node.size;
        merged.emplace(key, std::move(entry));
    }
    result.ok = true;
    result.entries.reserve(merged.size());
    for (auto& [key, entry] : merged) result.entries.push_back(std::move(entry));
    return result;
}

MountReadResult WritableMountView::read_file(std::string_view path, std::uint64_t offset,
                                             std::span<std::byte> out)
{
    const auto target = resolve(path);
    MountReadResult result;
    if (target.ok && !target.partition.empty()) {
        if (!target.exists) {
            result.error = "Mounted path does not exist";
            return result;
        }
        std::size_t bytes = 0;
        if (engine_.read_pending(target.partition, target.pfs_path, offset, out, bytes)) {
            result.ok = true;
            result.bytes_read = bytes;
            return result;
        }
    }
    return base_.read_file(path, offset, out);
}

} // namespace ps2hdd
