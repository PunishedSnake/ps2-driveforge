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
    ++apa_scans_;
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
    ++browse_operations_;
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

pfs::ExportResult DriveSession::export_to_host(std::string_view partition_id,
                                               std::string_view path,
                                               const std::filesystem::path& destination,
                                               pfs::ExportProgress progress)
{
    ++export_operations_;
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
    return {instrumented_.stats(), apa_scans_, browse_operations_, export_operations_};
}

void DriveSession::reset_stats() noexcept
{
    instrumented_.reset_stats();
    apa_scans_ = 0;
    browse_operations_ = 0;
    export_operations_ = 0;
}

} // namespace ps2hdd
