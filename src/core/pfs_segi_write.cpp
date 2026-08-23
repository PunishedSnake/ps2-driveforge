#include "ps2hdd/pfs_write.hpp"

#include "ps2hdd/pfs_extent_layout.hpp"
#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace ps2hdd::pfs {
namespace {

constexpr std::uint32_t kMetadataSectors =
    static_cast<std::uint32_t>(kMetadataSize / apa::kSectorSize);
constexpr std::size_t kMaxWriterSegi = 64;
constexpr std::size_t kMaxWriterDataExtents =
    (kInodeMaxBlocks - 1U) + kMaxWriterSegi * (kIndirectMaxBlocks - 1U);

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

bool needs_segi_path(std::string_view error) noexcept
{
    return error.find("SEGI") != std::string_view::npos ||
           error.find("indirect/unsupported") != std::string_view::npos ||
           error.find("indirect regular-file layout") != std::string_view::npos;
}

std::uint64_t zone_count(std::span<const ImageWriter::ZoneRun> runs) noexcept
{
    std::uint64_t total = 0;
    for (const auto& run : runs) {
        total += run.count;
    }
    return total;
}

std::vector<BlockInfo> as_blocks(std::span<const ImageWriter::ZoneRun> runs)
{
    std::vector<BlockInfo> blocks;
    blocks.reserve(runs.size());
    for (const auto& run : runs) {
        if (run.subpart > std::numeric_limits<std::uint16_t>::max() ||
            run.count > std::numeric_limits<std::uint16_t>::max()) {
            return {};
        }
        blocks.push_back({run.first, static_cast<std::uint16_t>(run.subpart),
                          static_cast<std::uint16_t>(run.count)});
    }
    return blocks;
}

void store_u16(std::byte* target, std::uint16_t value) noexcept
{
    std::memcpy(target, &value, sizeof(value));
}

} // namespace

FileWriteResult ImageWriter::create_file_segi(Node parent,
                                               std::string_view full_path,
                                               std::string_view name,
                                               std::span<const std::byte> bytes,
                                               const FileWriteOptions& options)
{
    FileWriteResult result;
    result.disposition = FileWriteDisposition::created;
    result.path = std::string(full_path);
    result.bytes = bytes.size();

    auto slot_probe = plan_dentry_insert(parent, name, {1, 0, 1});
    if (!slot_probe.ok && slot_probe.error.find("no reusable dentry space") != std::string::npos) {
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
    if (data_zones64 > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "PFS file payload is too large for the bounded SEGI writer";
        return result;
    }
    const auto data_zones = static_cast<std::uint32_t>(data_zones64);

    const auto inode_run = find_free_run(1, parent.location.subpart, parent.location.number);
    if (inode_run.count != 1 || inode_run.subpart > 0xFFU) {
        result.error = error_.empty() ? "No free PFS zone is available for a new inode" : error_;
        return result;
    }
    if (!reserve_runs(std::span<const ZoneRun>(&inode_run, 1), result,
                      "reserve PFS SEGI file inode zone")) {
        return result;
    }

    std::vector<ZoneRun> data_runs;
    std::vector<ZoneRun> segi_runs;
    const auto cleanup = [&]() {
        const auto saved_error = result.error;
        std::vector<ZoneRun> all;
        all.push_back(inode_run);
        all.insert(all.end(), data_runs.begin(), data_runs.end());
        all.insert(all.end(), segi_runs.begin(), segi_runs.end());
        FileWriteResult release;
        if (!release_runs(all, release, "release abandoned PFS SEGI file allocation")) {
            result.warning = "SEGI create failed safely but reserved zones remain allocated: " +
                             release.error;
        }
        result.error = saved_error;
    };

    if (data_zones != 0) {
        data_runs = find_free_runs(data_zones, inode_run.subpart, inode_run.first + 1U,
                                   kMaxWriterDataExtents);
        if (data_runs.empty() || zone_count(data_runs) != data_zones) {
            result.error = error_.empty() ? "PFS free space cannot represent the requested file"
                                          : error_;
            cleanup();
            return result;
        }
        if (!reserve_runs(data_runs, result, "reserve PFS SEGI file data zones")) {
            cleanup();
            return result;
        }
    }

    const auto segi_count = required_segi_count(data_runs.size());
    if (segi_count > kMaxWriterSegi) {
        result.error = "PFS file would exceed the bounded SEGI descriptor limit";
        cleanup();
        return result;
    }
    for (std::size_t index = 0; index < segi_count; ++index) {
        const auto preferred = data_runs.empty() ? inode_run.first + 1U
                                                 : data_runs.back().first + data_runs.back().count;
        const auto run = find_free_run(1, data_runs.empty() ? inode_run.subpart
                                                            : data_runs.back().subpart,
                                       preferred);
        if (run.count != 1 || run.subpart > std::numeric_limits<std::uint16_t>::max()) {
            result.error = error_.empty() ? "No free PFS zone is available for a SEGI descriptor"
                                          : error_;
            cleanup();
            return result;
        }
        if (!reserve_runs(std::span<const ZoneRun>(&run, 1), result,
                          "reserve PFS SEGI metadata zone")) {
            cleanup();
            return result;
        }
        segi_runs.push_back(run);
    }

    const auto data_blocks = as_blocks(data_runs);
    const auto segi_blocks = as_blocks(segi_runs);
    if (data_blocks.size() != data_runs.size() || segi_blocks.size() != segi_runs.size()) {
        result.error = "PFS extent cannot be represented by on-disk BlockInfo fields";
        cleanup();
        return result;
    }

    Inode base{};
    base.magic = kSegdMagic;
    base.inode_block = {inode_run.first, static_cast<std::uint16_t>(inode_run.subpart), 1};
    base.mode = options.mode;
    base.uid = options.uid;
    base.gid = options.gid;
    base.atime = options.timestamp;
    base.ctime = options.timestamp;
    base.mtime = options.timestamp;
    base.size = bytes.size();

    const auto layout = build_file_extent_layout(base, data_blocks, segi_blocks);
    if (!layout.ok) {
        result.error = layout.error;
        cleanup();
        return result;
    }
    const auto dentry = plan_dentry_insert(parent, name, layout.inode.inode_block);
    if (!dentry.ok) {
        result.error = dentry.error;
        cleanup();
        return result;
    }

    if (!write_payload_runs(data_runs, bytes, options.io_batch_bytes, result) ||
        !verify_payload_runs(data_runs, bytes, options.io_batch_bytes)) {
        if (result.error.empty()) {
            result.error = "PFS SEGI payload verification failed";
        }
        cleanup();
        return result;
    }

    if (!layout.segments.empty()) {
        WriteTransaction segment_tx(device_);
        for (const auto& segment : layout.segments) {
            const auto metadata_block64 = static_cast<std::uint64_t>(segment.location.number) << inode_scale();
            const auto sector64 = metadata_block64 * kMetadataSectors;
            std::uint64_t offset = 0;
            if (sector64 > std::numeric_limits<std::uint32_t>::max() ||
                !writable_volume_.absolute_byte_offset(segment.location.subpart,
                                                       static_cast<std::uint32_t>(sector64),
                                                       kMetadataSectors, offset) ||
                !segment_tx.stage(offset, std::as_bytes(std::span{&segment.descriptor, 1}),
                                  "write unreachable PFS SEGI descriptor")) {
                result.error = "Could not stage PFS SEGI metadata: " + segment_tx.stage_error();
                cleanup();
                return result;
            }
        }
        const auto committed = segment_tx.commit();
        if (!committed.ok) {
            result.error = "PFS SEGI metadata transaction failed: " + committed.error;
            cleanup();
            return result;
        }
        ++result.metadata_transactions;
    }

    const auto inode_block64 = static_cast<std::uint64_t>(layout.inode.inode_block.number) << inode_scale();
    const auto inode_sector64 = inode_block64 * kMetadataSectors;
    std::uint64_t inode_offset = 0;
    std::uint64_t dentry_offset = 0;
    if (inode_sector64 > std::numeric_limits<std::uint32_t>::max() ||
        !writable_volume_.absolute_byte_offset(layout.inode.inode_block.subpart,
                                               static_cast<std::uint32_t>(inode_sector64),
                                               kMetadataSectors, inode_offset) ||
        !writable_volume_.absolute_byte_offset(dentry.subpart, dentry.sector, 1, dentry_offset)) {
        result.error = "PFS SEGI file publication maps outside its APA extent";
        cleanup();
        return result;
    }

    WriteTransaction publication(device_);
    if (!publication.stage(inode_offset, std::as_bytes(std::span{&layout.inode, 1}),
                           "publish PFS SEGI inode") ||
        !publication.stage(dentry_offset, dentry.sector_bytes,
                           "publish PFS SEGI directory entry")) {
        result.error = "Could not stage PFS SEGI publication: " + publication.stage_error();
        cleanup();
        return result;
    }
    const auto committed = publication.commit([&]() -> std::string {
        return verify_file(full_path, bytes) ? std::string{}
                                             : std::string{"PFS reader did not verify SEGI file"};
    });
    if (!committed.ok) {
        result.error = "PFS SEGI publication failed: " + committed.error;
        cleanup();
        return result;
    }
    ++result.metadata_transactions;
    result.ok = true;
    return result;
}

FileWriteResult ImageWriter::replace_file_segi(const Node& node,
                                                std::string_view full_path,
                                                std::span<const std::byte> bytes,
                                                const FileWriteOptions& options)
{
    FileWriteResult result;
    result.disposition = FileWriteDisposition::replaced;
    result.path = std::string(full_path);
    result.bytes = bytes.size();
    if ((node.inode.mode & kModeMask) != kModeRegular) {
        result.error = "PFS SEGI replacement target is not a regular file";
        return result;
    }

    Reader reader(read_volume_, probe_);
    const auto old_map = reader.extent_map(node);
    if (!old_map) {
        result.error = "Could not inspect existing PFS extent graph: " + reader.last_error();
        return result;
    }

    const auto zone_size = static_cast<std::uint64_t>(probe_.super.zone_size);
    const auto data_zones64 = (bytes.size() + zone_size - 1U) / zone_size;
    if (data_zones64 > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "PFS replacement payload is too large for bounded SEGI writer";
        return result;
    }
    const auto data_zones = static_cast<std::uint32_t>(data_zones64);

    std::vector<ZoneRun> data_runs;
    std::vector<ZoneRun> segi_runs;
    const auto cleanup_new = [&]() {
        const auto saved_error = result.error;
        std::vector<ZoneRun> all = data_runs;
        all.insert(all.end(), segi_runs.begin(), segi_runs.end());
        if (!all.empty()) {
            FileWriteResult release;
            if (!release_runs(all, release, "release abandoned PFS SEGI replacement allocation")) {
                result.warning = "SEGI replacement failed safely but new zones remain allocated: " +
                                 release.error;
            }
        }
        result.error = saved_error;
    };

    if (data_zones != 0) {
        const auto preferred_sub = old_map->data.empty() ? node.location.subpart
                                                        : old_map->data.front().subpart;
        const auto preferred_zone = old_map->data.empty() ? node.location.number
                                                          : old_map->data.front().number;
        data_runs = find_free_runs(data_zones, preferred_sub, preferred_zone,
                                   kMaxWriterDataExtents);
        if (data_runs.empty() || zone_count(data_runs) != data_zones) {
            result.error = error_.empty() ? "PFS free space cannot represent replacement payload"
                                          : error_;
            return result;
        }
        if (!reserve_runs(data_runs, result, "reserve PFS SEGI replacement data zones")) {
            return result;
        }
    }

    const auto segi_count = required_segi_count(data_runs.size());
    if (segi_count > kMaxWriterSegi) {
        result.error = "PFS replacement exceeds bounded SEGI descriptor limit";
        cleanup_new();
        return result;
    }
    for (std::size_t index = 0; index < segi_count; ++index) {
        const auto run = find_free_run(1, node.location.subpart, node.location.number);
        if (run.count != 1 || run.subpart > std::numeric_limits<std::uint16_t>::max()) {
            result.error = error_.empty() ? "No free PFS zone is available for replacement SEGI metadata"
                                          : error_;
            cleanup_new();
            return result;
        }
        if (!reserve_runs(std::span<const ZoneRun>(&run, 1), result,
                          "reserve PFS replacement SEGI metadata zone")) {
            cleanup_new();
            return result;
        }
        segi_runs.push_back(run);
    }

    const auto data_blocks = as_blocks(data_runs);
    const auto segi_blocks = as_blocks(segi_runs);
    Inode base = node.inode;
    base.size = bytes.size();
    base.atime = options.timestamp;
    base.mtime = options.timestamp;
    const auto layout = build_file_extent_layout(base, data_blocks, segi_blocks);
    if (!layout.ok) {
        result.error = layout.error;
        cleanup_new();
        return result;
    }

    if (!write_payload_runs(data_runs, bytes, options.io_batch_bytes, result) ||
        !verify_payload_runs(data_runs, bytes, options.io_batch_bytes)) {
        if (result.error.empty()) {
            result.error = "PFS replacement SEGI payload verification failed";
        }
        cleanup_new();
        return result;
    }

    if (!layout.segments.empty()) {
        WriteTransaction segment_tx(device_);
        for (const auto& segment : layout.segments) {
            const auto metadata_block64 = static_cast<std::uint64_t>(segment.location.number) << inode_scale();
            const auto sector64 = metadata_block64 * kMetadataSectors;
            std::uint64_t offset = 0;
            if (sector64 > std::numeric_limits<std::uint32_t>::max() ||
                !writable_volume_.absolute_byte_offset(segment.location.subpart,
                                                       static_cast<std::uint32_t>(sector64),
                                                       kMetadataSectors, offset) ||
                !segment_tx.stage(offset, std::as_bytes(std::span{&segment.descriptor, 1}),
                                  "write unreachable replacement PFS SEGI descriptor")) {
                result.error = "Could not stage replacement PFS SEGI metadata: " + segment_tx.stage_error();
                cleanup_new();
                return result;
            }
        }
        const auto committed = segment_tx.commit();
        if (!committed.ok) {
            result.error = "Replacement PFS SEGI metadata transaction failed: " + committed.error;
            cleanup_new();
            return result;
        }
        ++result.metadata_transactions;
    }

    const auto inode_block64 = static_cast<std::uint64_t>(layout.inode.inode_block.number) << inode_scale();
    const auto inode_sector64 = inode_block64 * kMetadataSectors;
    std::uint64_t inode_offset = 0;
    if (inode_sector64 > std::numeric_limits<std::uint32_t>::max() ||
        !writable_volume_.absolute_byte_offset(layout.inode.inode_block.subpart,
                                               static_cast<std::uint32_t>(inode_sector64),
                                               kMetadataSectors, inode_offset)) {
        result.error = "PFS SEGI replacement inode maps outside APA extent";
        cleanup_new();
        return result;
    }

    WriteTransaction publication(device_);
    if (!publication.stage(inode_offset, std::as_bytes(std::span{&layout.inode, 1}),
                           "switch PFS inode to SEGI replacement")) {
        result.error = "Could not stage PFS SEGI replacement inode: " + publication.stage_error();
        cleanup_new();
        return result;
    }
    const auto committed = publication.commit([&]() -> std::string {
        return verify_file(full_path, bytes) ? std::string{}
                                             : std::string{"PFS reader did not verify SEGI replacement"};
    });
    if (!committed.ok) {
        result.error = "PFS SEGI replacement publication failed: " + committed.error;
        cleanup_new();
        return result;
    }
    ++result.metadata_transactions;

    std::vector<ZoneRun> old_allocation;
    old_allocation.reserve(old_map->data.size() + old_map->indirect_descriptors.size());
    for (const auto& block : old_map->data) {
        old_allocation.push_back({block.subpart, block.number, block.count});
    }
    for (const auto& block : old_map->indirect_descriptors) {
        old_allocation.push_back({block.subpart, block.number, 1});
    }
    if (!old_allocation.empty()) {
        FileWriteResult release;
        if (!release_runs(old_allocation, release, "release old PFS data and SEGI metadata")) {
            result.warning = "Replacement is valid but old PFS allocation remains reserved: " +
                             release.error;
            result.ok = true;
            return result;
        }
        result.metadata_transactions += release.metadata_transactions;
        result.bitmap_chunks_touched += release.bitmap_chunks_touched;
    }

    result.ok = true;
    return result;
}

FileWriteResult ImageWriter::write_file_complete(std::string_view input_path,
                                                  std::span<const std::byte> bytes,
                                                  const FileWriteOptions& options)
{
    auto result = write_file_full(input_path, bytes, options);
    if (result.ok || !needs_segi_path(result.error)) {
        return result;
    }

    const auto path = normalize_path(input_path);
    if (path.empty() || path == "/" || contains_parent_escape(path)) {
        return result;
    }
    const auto slash = path.find_last_of('/');
    const std::string parent_path = slash == std::string::npos || slash == 0
                                        ? "/"
                                        : path.substr(0, slash);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1U);

    Reader reader(read_volume_, probe_);
    if (const auto existing = reader.resolve(path)) {
        auto fallback = replace_file_segi(*existing, path, bytes, options);
        fallback.metadata_transactions += result.metadata_transactions;
        fallback.bitmap_chunks_touched += result.bitmap_chunks_touched;
        return fallback;
    }
    auto parent = reader.resolve(parent_path);
    if (!parent || (parent->inode.mode & kModeMask) != kModeDirectory) {
        return result;
    }
    auto fallback = create_file_segi(*parent, path, name, bytes, options);
    fallback.metadata_transactions += result.metadata_transactions;
    fallback.bitmap_chunks_touched += result.bitmap_chunks_touched;
    return fallback;
}

FileRemoveResult ImageWriter::remove_file_full(std::string_view input_path)
{
    auto direct = remove_file(input_path);
    if (direct.ok || !needs_segi_path(direct.error)) {
        return direct;
    }

    FileRemoveResult result;
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
    const auto node = reader.resolve(path);
    if (!node || (node->inode.mode & kModeMask) != kModeRegular) {
        result.error = "PFS unlink target is not a regular file";
        return result;
    }
    const auto map = reader.extent_map(*node);
    if (!map) {
        result.error = "Could not inspect PFS extent graph before unlink: " + reader.last_error();
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
                result.error = "PFS unlink allocation graph is duplicated, invalid or bitmap-free";
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
        result.error = "PFS unlink dentry maps outside APA extent";
        return result;
    }
    WriteTransaction unlink(device_);
    if (!unlink.stage(dentry_offset, dentry.sector_bytes, "unlink PFS SEGI directory entry")) {
        result.error = "Could not stage PFS SEGI unlink: " + unlink.stage_error();
        return result;
    }
    const auto committed = unlink.commit([&]() -> std::string {
        Reader verify(read_volume_, probe_);
        if (verify.resolve(path)) {
            return "PFS path still resolves after SEGI unlink";
        }
        const auto verify_parent = verify.resolve(parent_path);
        if (!verify_parent) {
            return "PFS parent became unreadable after SEGI unlink";
        }
        (void)verify.list_directory(*verify_parent, true);
        return verify.last_error();
    });
    if (!committed.ok) {
        result.error = "PFS SEGI unlink transaction failed: " + committed.error;
        return result;
    }
    ++result.metadata_transactions;

    FileWriteResult release_stats;
    if (!release_runs(release, release_stats, "release unlinked PFS inode data and SEGI zones")) {
        result.warning = "File is unlinked but its PFS allocation remains reserved: " +
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

} // namespace ps2hdd::pfs
