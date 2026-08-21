#include "ps2hdd/drive_session.hpp"

#include "ps2hdd/apa_volume.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

namespace ps2hdd {
namespace {

class AtomicTimer final {
public:
    explicit AtomicTimer(std::atomic<std::uint64_t>& destination)
        : destination_(destination), started_(std::chrono::steady_clock::now())
    {
    }

    ~AtomicTimer()
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started_).count();
        if (elapsed > 0) {
            destination_.fetch_add(static_cast<std::uint64_t>(elapsed), std::memory_order_relaxed);
        }
    }

    AtomicTimer(const AtomicTimer&) = delete;
    AtomicTimer& operator=(const AtomicTimer&) = delete;

private:
    std::atomic<std::uint64_t>& destination_;
    std::chrono::steady_clock::time_point started_;
};

std::string normalize_cache_path(std::string_view path)
{
    std::string normalized;
    normalized.reserve(path.size());
    bool previous_slash = true;
    for (char ch : path) {
        const char mapped = ch == '\\' ? '/' : ch;
        if (mapped == '/') {
            if (!previous_slash) {
                normalized.push_back('/');
            }
            previous_slash = true;
        } else {
            normalized.push_back(mapped);
            previous_slash = false;
        }
    }
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

std::string make_cache_key(std::string_view partition, std::string_view path)
{
    std::string key(partition);
    key.push_back('\x1f');
    key += normalize_cache_path(path);
    return key;
}

std::string child_path(std::string_view parent, std::string_view name)
{
    auto path = normalize_cache_path(parent);
    if (!path.empty()) {
        path.push_back('/');
    }
    path.append(name);
    return path;
}

StatResult stat_from_node(std::string_view path, const pfs::Node& node)
{
    StatResult result;
    result.entry.name = std::string(path);
    result.entry.inode = node.location;
    result.entry.mode = static_cast<std::uint16_t>(node.inode.mode & pfs::kModeMask);
    result.entry.size = node.inode.size;
    result.entry.inode_readable = true;
    result.ok = true;
    return result;
}

template <typename Map, typename Value>
void bounded_store(Map& map, std::string key, Value value, std::size_t max_entries,
                   std::atomic<std::uint64_t>& evictions)
{
    if (!map.contains(key) && map.size() >= max_entries && !map.empty()) {
        map.erase(map.begin());
        evictions.fetch_add(1, std::memory_order_relaxed);
    }
    map.insert_or_assign(std::move(key), std::move(value));
}

} // namespace

DriveSession::DriveSession(std::unique_ptr<BlockDevice> source)
    : source_(std::move(source)), instrumented_(*source_)
{
}

bool DriveSession::scan()
{
    AtomicTimer timer(scan_time_ns_);
    clear_caches();
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
    AtomicTimer timer(browse_time_ns_);
    browse_operations_.fetch_add(1, std::memory_order_relaxed);
    const auto normalized_path = normalize_cache_path(path);
    const auto key = make_cache_key(partition_id, normalized_path);

    {
        std::shared_lock lock(cache_mutex_);
        const auto cached = browse_cache_.find(key);
        if (cached != browse_cache_.end()) {
            browse_cache_hits_.fetch_add(1, std::memory_order_relaxed);
            return cached->second;
        }
    }
    browse_cache_misses_.fetch_add(1, std::memory_order_relaxed);

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

    auto node = reader.resolve(normalized_path);
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
    std::vector<std::pair<std::string, pfs::Node>> child_nodes;
    std::vector<std::pair<std::string, StatResult>> child_stats;
    child_nodes.reserve(entries.size() + 1);
    child_stats.reserve(entries.size() + 1);
    child_nodes.emplace_back(key, *node);
    child_stats.emplace_back(key, stat_from_node(normalized_path, *node));

    for (const auto& entry : entries) {
        SessionEntry visible;
        visible.name = entry.name;
        visible.inode = entry.inode;
        visible.mode = entry.mode;
        if (auto child = reader.read_inode(entry.inode)) {
            visible.inode_readable = true;
            visible.size = child->inode.size;
            visible.mode = static_cast<std::uint16_t>(child->inode.mode & pfs::kModeMask);
            const auto child_name = child_path(normalized_path, entry.name);
            const auto child_key = make_cache_key(partition_id, child_name);
            child_nodes.emplace_back(child_key, *child);
            child_stats.emplace_back(child_key, stat_from_node(child_name, *child));
        }
        result.entries.emplace_back(std::move(visible));
    }
    result.ok = true;

    {
        std::unique_lock lock(cache_mutex_);
        for (auto& [node_key, cached_node] : child_nodes) {
            bounded_store(node_cache_, std::move(node_key), std::move(cached_node),
                          kMaxNodeCacheEntries, cache_evictions_);
        }
        for (auto& [stat_key, cached_stat] : child_stats) {
            bounded_store(stat_cache_, std::move(stat_key), std::move(cached_stat),
                          kMaxStatCacheEntries, cache_evictions_);
        }
        bounded_store(browse_cache_, key, result, kMaxBrowseCacheEntries, cache_evictions_);
    }
    return result;
}

StatResult DriveSession::stat(std::string_view partition_id, std::string_view path)
{
    AtomicTimer timer(stat_time_ns_);
    stat_operations_.fetch_add(1, std::memory_order_relaxed);
    const auto normalized_path = normalize_cache_path(path);
    const auto key = make_cache_key(partition_id, normalized_path);

    {
        std::shared_lock lock(cache_mutex_);
        const auto cached = stat_cache_.find(key);
        if (cached != stat_cache_.end()) {
            stat_cache_hits_.fetch_add(1, std::memory_order_relaxed);
            return cached->second;
        }
    }
    stat_cache_misses_.fetch_add(1, std::memory_order_relaxed);

    {
        std::shared_lock lock(cache_mutex_);
        const auto cached = node_cache_.find(key);
        if (cached != node_cache_.end()) {
            node_cache_hits_.fetch_add(1, std::memory_order_relaxed);
            auto result = stat_from_node(normalized_path, cached->second);
            lock.unlock();
            std::unique_lock write_lock(cache_mutex_);
            bounded_store(stat_cache_, key, result, kMaxStatCacheEntries, cache_evictions_);
            return result;
        }
    }
    node_cache_misses_.fetch_add(1, std::memory_order_relaxed);

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
    auto node = reader.resolve(normalized_path);
    if (!node) {
        result.error = reader.last_error();
        return result;
    }

    result = stat_from_node(normalized_path, *node);
    {
        std::unique_lock lock(cache_mutex_);
        bounded_store(node_cache_, key, *node, kMaxNodeCacheEntries, cache_evictions_);
        bounded_store(stat_cache_, key, result, kMaxStatCacheEntries, cache_evictions_);
    }
    return result;
}

ReadResult DriveSession::read_file(std::string_view partition_id, std::string_view path,
                                   std::uint64_t offset, std::span<std::byte> out)
{
    AtomicTimer timer(read_time_ns_);
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

    const auto normalized_path = normalize_cache_path(path);
    const auto key = make_cache_key(partition_id, normalized_path);
    ApaVolume volume(instrumented_, *partition);
    pfs::Reader reader(volume);
    if (!reader.valid()) {
        result.error = reader.last_error().empty() ? "PFS probe failed" : reader.last_error();
        return result;
    }

    std::optional<pfs::Node> node;
    {
        std::shared_lock lock(cache_mutex_);
        const auto cached = node_cache_.find(key);
        if (cached != node_cache_.end()) {
            node = cached->second;
        }
    }
    if (node) {
        node_cache_hits_.fetch_add(1, std::memory_order_relaxed);
    } else {
        node_cache_misses_.fetch_add(1, std::memory_order_relaxed);
        node = reader.resolve(normalized_path);
        if (!node) {
            result.error = reader.last_error();
            return result;
        }
        std::unique_lock lock(cache_mutex_);
        bounded_store(node_cache_, key, *node, kMaxNodeCacheEntries, cache_evictions_);
        bounded_store(stat_cache_, key, stat_from_node(normalized_path, *node),
                      kMaxStatCacheEntries, cache_evictions_);
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
    AtomicTimer timer(export_time_ns_);
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
        {
            browse_cache_hits_.load(std::memory_order_relaxed),
            browse_cache_misses_.load(std::memory_order_relaxed),
            stat_cache_hits_.load(std::memory_order_relaxed),
            stat_cache_misses_.load(std::memory_order_relaxed),
            node_cache_hits_.load(std::memory_order_relaxed),
            node_cache_misses_.load(std::memory_order_relaxed),
            cache_evictions_.load(std::memory_order_relaxed),
        },
        apa_scans_.load(std::memory_order_relaxed),
        browse_operations_.load(std::memory_order_relaxed),
        stat_operations_.load(std::memory_order_relaxed),
        read_operations_.load(std::memory_order_relaxed),
        export_operations_.load(std::memory_order_relaxed),
        scan_time_ns_.load(std::memory_order_relaxed),
        browse_time_ns_.load(std::memory_order_relaxed),
        stat_time_ns_.load(std::memory_order_relaxed),
        read_time_ns_.load(std::memory_order_relaxed),
        export_time_ns_.load(std::memory_order_relaxed),
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
    scan_time_ns_.store(0, std::memory_order_relaxed);
    browse_time_ns_.store(0, std::memory_order_relaxed);
    stat_time_ns_.store(0, std::memory_order_relaxed);
    read_time_ns_.store(0, std::memory_order_relaxed);
    export_time_ns_.store(0, std::memory_order_relaxed);
    browse_cache_hits_.store(0, std::memory_order_relaxed);
    browse_cache_misses_.store(0, std::memory_order_relaxed);
    stat_cache_hits_.store(0, std::memory_order_relaxed);
    stat_cache_misses_.store(0, std::memory_order_relaxed);
    node_cache_hits_.store(0, std::memory_order_relaxed);
    node_cache_misses_.store(0, std::memory_order_relaxed);
    cache_evictions_.store(0, std::memory_order_relaxed);
}

void DriveSession::clear_caches()
{
    std::unique_lock lock(cache_mutex_);
    browse_cache_.clear();
    stat_cache_.clear();
    node_cache_.clear();
}

} // namespace ps2hdd
