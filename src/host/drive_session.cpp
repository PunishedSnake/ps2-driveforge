#include "ps2hdd/drive_session.hpp"

#include "ps2hdd/apa_volume.hpp"

#include <algorithm>
#include <utility>

namespace ps2hdd {

DriveSession::DriveSession(std::unique_ptr<BlockDevice> source)
    : source_(std::move(source)), instrumented_(*source_)
{
}

bool DriveSession::scan()
{
    last_error_.clear();
    apa_scans_.fetch_add(1, std::memory_order_relaxed);
    apa::Reader reader(instrumented_);
    scan_ = reader.scan();
    if (!scan_.mbr_valid) {
        last_error_ = "Source does not contain a valid PS2 APA MBR";
        return false;
    }
    if (!scan_.ok()) {
        last_error_ = "APA diagnostics contain fatal errors";
        return false;
    }
    return true;
}

const apa::Partition* DriveSession::find_partition(std::string_view id) const noexcept
{
    const auto it = std::find_if(scan_.partitions.begin(), scan_.partitions.end(),
                                 [id](const apa::Partition& partition) {
                                     return !partition.is_sub() && partition.id == id;
                                 });
    return it == scan_.partitions.end() ? nullptr : &*it;
}

BrowseResult DriveSession::browse(std::string_view partition_id, std::string_view path)
{
    browse_operations_.fetch_add(1, std::memory_order_relaxed);
    BrowseResult result;
    const auto* partition = find_partition(partition_id);
    if (!partition) {
        result.error = "Partition not found: " + std::string(partition_id);
        return result;
    }
    if (partition->type != apa::kTypePfs) {
        result.error = "Partition is not PFS: " + std::string(partition_id);
        return result;
    }

    ApaVolume volume(instrumented_, *partition);
    pfs::Reader reader(volume);
    if (!reader.valid()) {
        result.error = reader.last_error().empty() ? "PFS probe failed" : reader.last_error();
        return result;
    }

    auto node = reader.resolve(path);
    if (!node) {
        result.error = reader.last_error();
        return result;
    }
    if ((node->inode.mode & pfs::kModeMask) != pfs::kModeDirectory) {
        result.error = "PFS path is not a directory";
        return result;
    }

    const auto entries = reader.list_directory(*node);
    if (!reader.last_error().empty()) {
        result.error = reader.last_error();
        return result;
    }

    result.entries.reserve(entries.size());
    for (const auto& entry : entries) {
        SessionEntry visible;
        visible.name = entry.name;
        visible.inode = entry.inode;
        visible.mode = entry.mode;
        if (auto child = reader.read_inode(entry.inode)) {
            visible.inode_readable = true;
            visible.size = child->inode.size;
            visible.mode = static_cast<std::uint16_t>(child->inode.mode & pfs::kModeMask);
        }
        result.entries.emplace_back(std::move(visible));
    }
    result.ok = true;
    return result;
}

StatResult DriveSession::stat(std::string_view partition_id, std::string_view path)
{
    stat_operations_.fetch_add(1, std::memory_order_relaxed);
    StatResult result;
    const auto* partition = find_partition(partition_id);
    if (!partition) {
        result.error = "Partition not found: " + std::string(partition_id);
        return result;
    }
    if (partition->type != apa::kTypePfs) {
        result.error = "Partition is not PFS: " + std::string(partition_id);
        return result;
    }

    ApaVolume volume(instrumented_, *partition);
    pfs::Reader reader(volume);
    if (!reader.valid()) {
        result.error = reader.last_error().empty() ? "PFS probe failed" : reader.last_error();
        return result;
    }
    auto node = reader.resolve(path);
    if (!node) {
        result.error = reader.last_error();
        return result;
    }

    result.entry.name = std::string(path);
    result.entry.inode = node->location;
    result.entry.mode = static_cast<std::uint16_t>(node->inode.mode & pfs::kModeMask);
    result.entry.size = node->inode.size;
    result.entry.inode_readable = true;
    result.ok = true;
    return result;
}

ReadResult DriveSession::read_file(std::string_view partition_id, std::string_view path,
                                   std::uint64_t offset, std::span<std::byte> out)
{
    read_operations_.fetch_add(1, std::memory_order_relaxed);
    ReadResult result;
    const auto* partition = find_partition(partition_id);
    if (!partition) {
        result.error = "Partition not found: " + std::string(partition_id);
        return result;
    }
    if (partition->type != apa::kTypePfs) {
        result.error = "Partition is not PFS: " + std::string(partition_id);
        return result;
    }

    ApaVolume volume(instrumented_, *partition);
    pfs::Reader reader(volume);
    if (!reader.valid()) {
        result.error = reader.last_error().empty() ? "PFS probe failed" : reader.last_error();
        return result;
    }
    auto node = reader.resolve(path);
    if (!node) {
        result.error = reader.last_error();
        return result;
    }
    if ((node->inode.mode & pfs::kModeMask) != pfs::kModeRegular) {
        result.error = "PFS path is not a regular file";
        return result;
    }

    // Windows ReadFile requests beyond EOF are normal. Clamp the requested span
    // here instead of weakening pfs::Reader::read(), whose strict bounds check is
    // still useful for parser correctness and corruption detection.
    if (offset >= node->inode.size || out.empty()) {
        result.ok = true;
        return result;
    }
    const auto take = static_cast<std::size_t>(
        std::min<std::uint64_t>(out.size(), node->inode.size - offset));
    if (!reader.read(*node, offset, out.first(take))) {
        result.error = reader.last_error();
        return result;
    }
    result.bytes_read = take;
    result.ok = true;
    return result;
}

pfs::ExportResult DriveSession::export_to_host(std::string_view partition_id,
                                               std::string_view path,
                                               const std::filesystem::path& destination,
                                               pfs::ExportProgress progress)
{
    export_operations_.fetch_add(1, std::memory_order_relaxed);
    pfs::ExportResult error_result;

    const auto* partition = find_partition(partition_id);
    if (!partition) {
        error_result.error = "Partition not found: " + std::string(partition_id);
        return error_result;
    }
    if (partition->type != apa::kTypePfs) {
        error_result.error = "Partition is not PFS: " + std::string(partition_id);
        return error_result;
    }

    ApaVolume volume(instrumented_, *partition);
    pfs::Reader reader(volume);
    if (!reader.valid()) {
        error_result.error = reader.last_error().empty() ? "PFS probe failed" : reader.last_error();
        return error_result;
    }
    return pfs::export_to_host(reader, path, destination, std::move(progress));
}

SessionStats DriveSession::stats() const noexcept
{
    return {
        instrumented_.stats(),
        apa_scans_.load(std::memory_order_relaxed),
        browse_operations_.load(std::memory_order_relaxed),
        stat_operations_.load(std::memory_order_relaxed),
        read_operations_.load(std::memory_order_relaxed),
        export_operations_.load(std::memory_order_relaxed),
    };
}

void DriveSession::reset_stats() noexcept
{
    instrumented_.reset_stats();
    apa_scans_.store(0, std::memory_order_relaxed);
    browse_operations_.store(0, std::memory_order_relaxed);
    stat_operations_.store(0, std::memory_order_relaxed);
    read_operations_.store(0, std::memory_order_relaxed);
    export_operations_.store(0, std::memory_order_relaxed);
}

} // namespace ps2hdd
