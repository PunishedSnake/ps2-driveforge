#include "ps2hdd/pfs_write_back.hpp"

#include "ps2hdd/pfs_write.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace ps2hdd {
namespace {

constexpr std::size_t kCopyChunk = 1024U * 1024U;

std::string upper_ascii(std::string_view value)
{
    std::string out(value);
    for (char& ch : out) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string normalize(std::string_view path)
{
    std::string out = "/";
    std::size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && (path[i] == '/' || path[i] == '\\')) ++i;
        const auto start = i;
        while (i < path.size() && path[i] != '/' && path[i] != '\\') ++i;
        if (i > start) {
            if (out.size() > 1) out += '/';
            out.append(path.substr(start, i - start));
        }
    }
    return out;
}

std::string parent_of(const std::string& path)
{
    const auto slash = path.rfind('/');
    return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

std::string leaf_of(const std::string& path)
{
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string make_key(std::string_view partition, const std::string& path)
{
    std::string key(partition);
    key += '\x1f';
    key += upper_ascii(path);
    return key;
}

enum class OpType { write, remove, mkdir, rmdir };

struct Op {
    OpType type{OpType::write};
    std::uint64_t seq{};
    std::string partition;
    std::string path;
    std::filesystem::path spool;
    std::uint64_t size{};
    bool existed_on_disk{};
};

struct HandleState {
    std::string partition;
    std::string path;
    std::filesystem::path spool;
    std::uint64_t size{};
    bool dirty{};
};

enum class Kind { none, file, directory };

} // namespace

const char* write_status_name(WriteStatus status) noexcept
{
    switch (status) {
    case WriteStatus::ok: return "ok";
    case WriteStatus::not_found: return "not found";
    case WriteStatus::exists: return "already exists";
    case WriteStatus::is_directory: return "is a directory";
    case WriteStatus::not_directory: return "not a directory";
    case WriteStatus::not_empty: return "directory not empty";
    case WriteStatus::read_only: return "write-back is read-only (circuit breaker)";
    case WriteStatus::protected_partition: return "partition is write-protected";
    case WriteStatus::busy: return "write-back queue full";
    case WriteStatus::no_space: return "not enough PFS space (reserve kept free)";
    case WriteStatus::too_large: return "file exceeds whole-file commit limit";
    case WriteStatus::invalid: return "invalid request";
    case WriteStatus::io_error:
    default: return "I/O error";
    }
}

struct PfsWriteBack::Impl {
    Impl(DriveSession& s, WriterFactory f, WriteBackOptions o)
        : session(s), factory(std::move(f)), options(std::move(o))
    {
        std::error_code ec;
        std::filesystem::create_directories(options.spool_directory / "failed", ec);
        journal_path = options.spool_directory / "journal.log";
        if (PfsWriteBack::scan_journal(options.spool_directory).empty()) {
            // Clean shutdown last time: start a fresh journal and drop stale
            // handle spools (opens that never reached close are not intents).
            std::filesystem::remove(journal_path, ec);
            for (const auto& entry : std::filesystem::directory_iterator(options.spool_directory, ec)) {
                if (entry.is_regular_file() && entry.path().extension() == ".bin") {
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        }
        // Continue sequence numbers after any previous run so orphan spool
        // files from a crash are never overwritten.
        std::ifstream in(journal_path);
        std::string tag;
        std::uint64_t seq = 0;
        std::string rest;
        while (in >> tag >> seq) {
            next_seq = std::max(next_seq, seq + 1);
            std::getline(in, rest);
        }
        worker = std::thread([this] { run(); });
    }

    ~Impl()
    {
        {
            std::unique_lock lock(mutex);
            flush_requested = true;
        }
        cv.notify_all();
        {
            std::unique_lock lock(mutex);
            done_cv.wait_for(lock, std::chrono::seconds(120), [this] {
                return (pending.empty() && in_flight.empty()) || read_only;
            });
            stop = true;
        }
        cv.notify_all();
        worker.join();
    }

    // ---- helpers (mutex held) -------------------------------------------

    bool in_flight_seq(std::uint64_t seq) const { return in_flight.contains(seq); }

    // Disk state after every op currently queued or committing for this key.
    bool will_exist_on_disk(const Op& op) const
    {
        if (in_flight_seq(op.seq)) {
            return op.type == OpType::write || op.type == OpType::mkdir;
        }
        return op.existed_on_disk;
    }

    void journal(const std::string& line)
    {
        std::ofstream out(journal_path, std::ios::app);
        out << line << '\n';
        out.flush();
    }

    void drop_spool(const Op& op)
    {
        if (op.type == OpType::write && !op.spool.empty() && !in_flight_seq(op.seq)) {
            std::error_code ec;
            std::filesystem::remove(op.spool, ec);
        }
    }

    Kind disk_kind(std::string_view partition, const std::string& path)
    {
        if (path == "/") return Kind::directory;
        const auto stat = session.stat(partition, path);
        if (!stat.ok) return Kind::none;
        return stat.entry.is_directory() ? Kind::directory : Kind::file;
    }

    Kind effective_kind(std::string_view partition, const std::string& path)
    {
        if (const auto it = pending.find(make_key(partition, path)); it != pending.end()) {
            switch (it->second.type) {
            case OpType::write: return Kind::file;
            case OpType::mkdir: return Kind::directory;
            case OpType::remove:
            case OpType::rmdir: return Kind::none;
            }
        }
        return disk_kind(partition, path);
    }

    std::uint64_t pending_write_bytes() const
    {
        std::uint64_t total = 0;
        for (const auto& [key, op] : pending) {
            if (op.type == OpType::write) total += op.size;
        }
        for (const auto& [id, handle] : handles) total += handle.size;
        return total;
    }

    std::uint64_t alloc_bytes(std::uint64_t size) const
    {
        const auto zone = zone_size == 0 ? 8192U : zone_size;
        return (size + zone - 1) / zone * zone + zone; // payload + inode/SEGI zone
    }

    void enqueue(Op op)
    {
        const auto key = make_key(op.partition, op.path);
        if (const auto it = pending.find(key); it != pending.end()) {
            ++stats.coalesced_ops;
            drop_spool(it->second);
            it->second = std::move(op);
        } else {
            pending.emplace(key, std::move(op));
        }
        last_activity = std::chrono::steady_clock::now();
        cv.notify_all();
    }

    WriteStatus admit_mutation(std::string_view partition) const
    {
        if (read_only) return WriteStatus::read_only;
        if (!owner->partition_writable(partition)) return WriteStatus::protected_partition;
        if (pending.size() >= options.max_pending_ops) return WriteStatus::busy;
        return WriteStatus::ok;
    }

    bool copy_into(std::string_view partition, const std::string& path,
                   const std::filesystem::path& target, std::uint64_t& size)
    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const auto key = make_key(partition, path);
        if (const auto it = pending.find(key); it != pending.end() && it->second.type == OpType::write) {
            std::ifstream in(it->second.spool, std::ios::binary);
            out << in.rdbuf();
            size = it->second.size;
            return static_cast<bool>(out) || size == 0;
        }
        const auto stat = session.stat(partition, path);
        if (!stat.ok) return false;
        size = stat.entry.size;
        std::vector<std::byte> chunk(kCopyChunk);
        for (std::uint64_t offset = 0; offset < size; offset += chunk.size()) {
            const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), size - offset));
            const auto read = session.read_file(partition, path, offset, std::span(chunk).first(want));
            if (!read.ok || read.bytes_read != want) return false;
            out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(want));
        }
        return static_cast<bool>(out);
    }

    // ---- committer -------------------------------------------------------

    void run()
    {
        std::unique_lock lock(mutex);
        while (true) {
            if (read_only || pending.empty()) {
                done_cv.notify_all();
                cv.wait_for(lock, std::chrono::milliseconds(250), [this] {
                    return stop || (!read_only && !pending.empty());
                });
                if (stop) return;
                continue;
            }
            const auto quiet = std::chrono::steady_clock::now() - last_activity;
            const bool pressure = pending_write_bytes() >= options.max_pending_bytes / 2 ||
                                  pending.size() >= options.max_pending_ops / 2;
            if (!flush_requested && !pressure && quiet < options.commit_delay) {
                // Debounce: sleep exactly until the queue has been quiet long enough.
                cv.wait_for(lock, options.commit_delay - quiet, [this] { return stop || flush_requested; });
                if (stop) return;
                continue;
            }

            std::vector<Op> batch;
            for (const auto& [key, op] : pending) {
                if (!in_flight_seq(op.seq)) batch.push_back(op);
            }
            std::sort(batch.begin(), batch.end(), [](const Op& a, const Op& b) { return a.seq < b.seq; });
            if (batch.size() > options.max_ops_per_batch) batch.resize(options.max_ops_per_batch);
            for (const auto& op : batch) in_flight.insert(op.seq);

            lock.unlock();
            std::vector<std::pair<Op, bool>> outcome;
            std::string batch_error;
            commit(batch, outcome, batch_error);
            session.clear_caches();
            lock.lock();

            bool failed = !batch_error.empty();
            for (auto& [op, ok] : outcome) {
                in_flight.erase(op.seq);
                const auto key = make_key(op.partition, op.path);
                const auto it = pending.find(key);
                const bool still_current = it != pending.end() && it->second.seq == op.seq;
                if (ok) {
                    ++stats.committed_ops;
                    if (op.type == OpType::write) {
                        std::error_code ec;
                        std::filesystem::remove(op.spool, ec);
                    }
                    if (still_current) pending.erase(it);
                } else {
                    failed = true;
                    ++stats.failed_ops;
                    if (op.type == OpType::write) {
                        std::error_code ec;
                        std::filesystem::rename(op.spool, options.spool_directory / "failed" / op.spool.filename(), ec);
                    }
                    if (still_current) pending.erase(it);
                }
            }
            if (failed) {
                ++stats.failed_batches;
                ++consecutive_failures;
                stats.last_error = batch_error.empty() ? last_op_error : batch_error;
                if (consecutive_failures >= options.breaker_failed_batches) {
                    read_only = true;
                    // Stop presenting uncommitted changes as if they will land.
                    // Their spool files and W intents stay, so the journal can
                    // replay them once the operator has fixed the cause.
                    for (auto pit = pending.begin(); pit != pending.end();) {
                        if (in_flight_seq(pit->second.seq)) { ++pit; continue; }
                        pit = pending.erase(pit);
                    }
                    stats.last_error = "Circuit breaker: " + std::to_string(consecutive_failures) +
                                       " consecutive failed batches; write-back is now read-only. Last error: " +
                                       stats.last_error;
                }
            } else {
                consecutive_failures = 0;
            }
            ++stats.committed_batches;
            if (pending.empty()) flush_requested = false;
            done_cv.notify_all();

            lock.unlock();
            std::this_thread::sleep_for(options.batch_pause);
            lock.lock();
        }
    }

    void commit(const std::vector<Op>& batch, std::vector<std::pair<Op, bool>>& outcome,
                std::string& batch_error)
    {
        std::string error;
        auto device = factory ? factory(error) : nullptr;
        {
            std::scoped_lock lock(mutex);
            ++stats.disk_write_sessions;
        }
        if (!device) {
            batch_error = error.empty() ? "Could not open write capability" : error;
            for (const auto& op : batch) outcome.emplace_back(op, false);
            return;
        }

        std::map<std::string, std::unique_ptr<pfs::ImageWriter>> writers;
        std::map<std::string, std::optional<std::uint64_t>> free_space;
        for (const auto& op : batch) {
            auto& writer = writers[op.partition];
            if (!writer) {
                const auto* partition = session.find_partition(op.partition);
                if (partition == nullptr) {
                    remember_error("Partition vanished: " + op.partition);
                    outcome.emplace_back(op, false);
                    continue;
                }
                writer = std::make_unique<pfs::ImageWriter>(*device, *partition);
                free_space[op.partition] = writer->valid() ? writer->free_bytes() : std::nullopt;
                if (writer->valid()) zone_size = writer->zone_bytes();
            }
            if (!writer->valid()) {
                remember_error("PFS writer refused " + op.partition + ": " + writer->error());
                outcome.emplace_back(op, false);
                continue;
            }

            bool ok = false;
            switch (op.type) {
            case OpType::write: {
                auto& free = free_space[op.partition];
                const auto needed = alloc_bytes(op.size);
                if (!free || *free < needed + options.free_space_reserve_bytes) {
                    remember_error("Not enough PFS space for " + op.path + " (reserve kept)");
                    break;
                }
                std::vector<std::byte> bytes(static_cast<std::size_t>(op.size));
                std::ifstream in(op.spool, std::ios::binary);
                if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) &&
                    !bytes.empty()) {
                    remember_error("Spool file unreadable for " + op.path);
                    break;
                }
                const auto result = writer->write_file_complete(op.path, bytes);
                ok = result.ok;
                if (!ok) remember_error(op.path + ": " + result.error);
                else *free -= std::min(*free, needed);
                break;
            }
            case OpType::remove: {
                const auto result = writer->remove_file_full(op.path);
                ok = result.ok;
                if (!ok) remember_error(op.path + ": " + result.error);
                break;
            }
            case OpType::mkdir: {
                const auto result = writer->ensure_directory_full(op.path);
                ok = result.ok;
                if (!ok) remember_error(op.path + ": " + result.error);
                break;
            }
            case OpType::rmdir: {
                const auto result = writer->remove_empty_directory(op.path);
                ok = result.ok;
                if (!ok) remember_error(op.path + ": " + result.error);
                break;
            }
            }
            {
                std::scoped_lock lock(mutex);
                journal((ok ? "C " : "F ") + std::to_string(op.seq));
            }
            outcome.emplace_back(op, ok);
        }
        std::scoped_lock lock(mutex);
        for (auto& [partition, writer] : writers) {
            if (writer && writer->valid()) {
                if (const auto free = writer->free_bytes()) free_cache[partition] = *free;
            }
        }
    }

    void remember_error(std::string error)
    {
        std::scoped_lock lock(mutex);
        last_op_error = std::move(error);
    }

    DriveSession& session;
    WriterFactory factory;
    WriteBackOptions options;
    PfsWriteBack* owner{};
    std::filesystem::path journal_path;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable done_cv;
    std::map<std::string, Op> pending;
    std::set<std::uint64_t> in_flight;
    std::unordered_map<Handle, HandleState> handles;
    std::map<std::string, std::uint64_t> free_cache;
    WriteBackStats stats;
    std::string last_op_error;
    std::uint64_t next_seq{1};
    Handle next_handle{1};
    std::uint64_t zone_size{};
    std::size_t consecutive_failures{};
    bool read_only{};
    bool flush_requested{};
    bool stop{};
    std::chrono::steady_clock::time_point last_activity{std::chrono::steady_clock::now()};
    std::thread worker;
};

PfsWriteBack::PfsWriteBack(DriveSession& session, WriterFactory factory, WriteBackOptions options)
    : impl_(std::make_unique<Impl>(session, std::move(factory), std::move(options)))
{
    impl_->owner = this;
}

PfsWriteBack::~PfsWriteBack() = default;

bool PfsWriteBack::partition_writable(std::string_view partition) const
{
    const std::string id(partition);
    const auto& o = impl_->options;
    if (!o.writable_partitions.empty()) return o.writable_partitions.contains(id);
    if (o.protected_partitions.contains(id)) return false;
    const auto* p = impl_->session.find_partition(partition);
    return p != nullptr && p->type == apa::kTypePfs;
}

WriteStatus PfsWriteBack::open_write(std::string_view partition, std::string_view raw_path,
                                     bool truncate, bool create_new, Handle& handle)
{
    auto& d = *impl_;
    const auto path = normalize(raw_path);
    if (path == "/") return WriteStatus::is_directory;
    std::unique_lock lock(d.mutex);
    if (const auto admitted = d.admit_mutation(partition); admitted != WriteStatus::ok) return admitted;

    const auto kind = d.effective_kind(partition, path);
    if (kind == Kind::directory) return WriteStatus::is_directory;
    if (kind == Kind::file && create_new) return WriteStatus::exists;
    if (kind == Kind::none && d.effective_kind(partition, parent_of(path)) != Kind::directory) {
        return WriteStatus::not_found;
    }

    HandleState state;
    state.partition = std::string(partition);
    state.path = path;
    state.spool = d.options.spool_directory / ("h" + std::to_string(d.next_handle) + ".bin");
    if (kind == Kind::file && !truncate) {
        if (!d.copy_into(partition, path, state.spool, state.size)) return WriteStatus::io_error;
    } else {
        std::ofstream create(state.spool, std::ios::binary | std::ios::trunc);
        if (!create) return WriteStatus::io_error;
        state.dirty = true; // creating or truncating is itself a change
    }
    handle = d.next_handle++;
    d.handles.emplace(handle, std::move(state));
    d.stats.open_handles = d.handles.size();
    return WriteStatus::ok;
}

WriteStatus PfsWriteBack::write(Handle handle, std::uint64_t offset, std::span<const std::byte> data)
{
    auto& d = *impl_;
    std::unique_lock lock(d.mutex);
    const auto it = d.handles.find(handle);
    if (it == d.handles.end()) return WriteStatus::invalid;
    if (d.read_only) return WriteStatus::read_only;
    auto& h = it->second;
    const auto end = offset + data.size();
    if (end > d.options.max_file_bytes) return WriteStatus::too_large;
    const auto growth = end > h.size ? end - h.size : 0;
    if (growth > 0) {
        if (d.pending_write_bytes() + growth > d.options.max_pending_bytes) return WriteStatus::busy;
        if (const auto free = d.free_cache.find(h.partition); free != d.free_cache.end()) {
            const auto queued = d.pending_write_bytes();
            if (free->second < d.options.free_space_reserve_bytes + queued + growth) return WriteStatus::no_space;
        }
    }
    std::fstream out(h.spool, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) return WriteStatus::io_error;
    if (offset > h.size) {
        // Sparse extension: fill the gap with zeros like NTFS would.
        out.seekp(static_cast<std::streamoff>(h.size));
        const std::vector<char> zeros(static_cast<std::size_t>(std::min<std::uint64_t>(offset - h.size, kCopyChunk)));
        for (std::uint64_t left = offset - h.size; left > 0;) {
            const auto n = std::min<std::uint64_t>(left, zeros.size());
            out.write(zeros.data(), static_cast<std::streamsize>(n));
            left -= n;
        }
    }
    out.seekp(static_cast<std::streamoff>(offset));
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!out) return WriteStatus::io_error;
    h.size = std::max(h.size, end);
    h.dirty = true;
    ++d.stats.application_writes;
    d.last_activity = std::chrono::steady_clock::now();
    return WriteStatus::ok;
}

WriteStatus PfsWriteBack::set_size(Handle handle, std::uint64_t size)
{
    auto& d = *impl_;
    std::unique_lock lock(d.mutex);
    const auto it = d.handles.find(handle);
    if (it == d.handles.end()) return WriteStatus::invalid;
    if (d.read_only) return WriteStatus::read_only;
    if (size > d.options.max_file_bytes) return WriteStatus::too_large;
    if (size > it->second.size &&
        d.pending_write_bytes() + (size - it->second.size) > d.options.max_pending_bytes) {
        return WriteStatus::busy;
    }
    std::error_code ec;
    std::filesystem::resize_file(it->second.spool, size, ec);
    if (ec) return WriteStatus::io_error;
    it->second.size = size;
    it->second.dirty = true;
    return WriteStatus::ok;
}

WriteStatus PfsWriteBack::read(Handle handle, std::uint64_t offset, std::span<std::byte> out,
                               std::size_t& bytes_read)
{
    auto& d = *impl_;
    std::unique_lock lock(d.mutex);
    bytes_read = 0;
    const auto it = d.handles.find(handle);
    if (it == d.handles.end()) return WriteStatus::invalid;
    if (offset >= it->second.size) return WriteStatus::ok;
    std::ifstream in(it->second.spool, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    const auto want = std::min<std::uint64_t>(out.size(), it->second.size - offset);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(want));
    bytes_read = static_cast<std::size_t>(in.gcount());
    return WriteStatus::ok;
}

std::optional<std::uint64_t> PfsWriteBack::handle_size(Handle handle) const
{
    std::unique_lock lock(impl_->mutex);
    const auto it = impl_->handles.find(handle);
    if (it == impl_->handles.end()) return std::nullopt;
    return it->second.size;
}

WriteStatus PfsWriteBack::close(Handle handle, bool discard)
{
    auto& d = *impl_;
    std::unique_lock lock(d.mutex);
    const auto it = d.handles.find(handle);
    if (it == d.handles.end()) return WriteStatus::invalid;
    HandleState h = std::move(it->second);
    d.handles.erase(it);
    d.stats.open_handles = d.handles.size();
    std::error_code ec;
    if (discard || !h.dirty || d.read_only) {
        std::filesystem::remove(h.spool, ec);
        return discard || !h.dirty ? WriteStatus::ok : WriteStatus::read_only;
    }

    Op op;
    op.type = OpType::write;
    op.seq = d.next_seq++;
    op.partition = h.partition;
    op.path = h.path;
    op.size = h.size;
    op.spool = d.options.spool_directory / ("w" + std::to_string(op.seq) + ".bin");
    std::filesystem::rename(h.spool, op.spool, ec);
    if (ec) return WriteStatus::io_error;
    const auto key = make_key(op.partition, op.path);
    if (const auto prev = d.pending.find(key); prev != d.pending.end()) {
        op.existed_on_disk = d.will_exist_on_disk(prev->second);
    } else {
        op.existed_on_disk = d.disk_kind(op.partition, op.path) == Kind::file;
    }
    d.journal("W " + std::to_string(op.seq) + " " + std::to_string(op.size) + " " +
              op.spool.filename().string() + " " + op.partition + "\t" + op.path);
    d.enqueue(std::move(op));
    return WriteStatus::ok;
}

WriteStatus PfsWriteBack::remove_file(std::string_view partition, std::string_view raw_path)
{
    auto& d = *impl_;
    const auto path = normalize(raw_path);
    std::unique_lock lock(d.mutex);
    if (const auto admitted = d.admit_mutation(partition); admitted != WriteStatus::ok) return admitted;
    const auto key = make_key(partition, path);
    if (const auto it = d.pending.find(key); it != d.pending.end()) {
        if (it->second.type != OpType::write) {
            return it->second.type == OpType::mkdir ? WriteStatus::is_directory : WriteStatus::not_found;
        }
        if (!d.will_exist_on_disk(it->second)) {
            ++d.stats.coalesced_ops;
            d.drop_spool(it->second);
            d.journal("C " + std::to_string(it->second.seq)); // retired without disk I/O
            d.pending.erase(it);
            return WriteStatus::ok;
        }
    } else {
        const auto kind = d.disk_kind(partition, path);
        if (kind == Kind::none) return WriteStatus::not_found;
        if (kind == Kind::directory) return WriteStatus::is_directory;
    }
    Op op;
    op.type = OpType::remove;
    op.seq = d.next_seq++;
    op.partition = std::string(partition);
    op.path = path;
    op.existed_on_disk = true;
    d.journal("D " + std::to_string(op.seq) + " " + op.partition + "\t" + op.path);
    d.enqueue(std::move(op));
    return WriteStatus::ok;
}

WriteStatus PfsWriteBack::make_directory(std::string_view partition, std::string_view raw_path)
{
    auto& d = *impl_;
    const auto path = normalize(raw_path);
    if (path == "/") return WriteStatus::exists;
    std::unique_lock lock(d.mutex);
    if (const auto admitted = d.admit_mutation(partition); admitted != WriteStatus::ok) return admitted;
    if (d.effective_kind(partition, parent_of(path)) != Kind::directory) return WriteStatus::not_found;
    const auto key = make_key(partition, path);
    if (const auto it = d.pending.find(key); it != d.pending.end()) {
        if (it->second.type == OpType::rmdir && !d.in_flight_seq(it->second.seq)) {
            ++d.stats.coalesced_ops;
            d.journal("C " + std::to_string(it->second.seq));
            d.pending.erase(it); // rmdir + mkdir of an existing directory cancels out
            return WriteStatus::ok;
        }
        if (it->second.type == OpType::write || it->second.type == OpType::mkdir) return WriteStatus::exists;
    } else if (d.disk_kind(partition, path) != Kind::none) {
        return WriteStatus::exists;
    }
    Op op;
    op.type = OpType::mkdir;
    op.seq = d.next_seq++;
    op.partition = std::string(partition);
    op.path = path;
    d.journal("M " + std::to_string(op.seq) + " " + op.partition + "\t" + op.path);
    d.enqueue(std::move(op));
    return WriteStatus::ok;
}

WriteStatus PfsWriteBack::remove_directory(std::string_view partition, std::string_view raw_path)
{
    auto& d = *impl_;
    const auto path = normalize(raw_path);
    if (path == "/") return WriteStatus::invalid;
    {
        std::unique_lock lock(d.mutex);
        if (const auto admitted = d.admit_mutation(partition); admitted != WriteStatus::ok) return admitted;
        const auto kind = d.effective_kind(partition, path);
        if (kind == Kind::none) return WriteStatus::not_found;
        if (kind == Kind::file) return WriteStatus::not_directory;
    }
    if (!pending_children(partition, path).empty()) return WriteStatus::not_empty;

    std::unique_lock lock(d.mutex);
    const auto key = make_key(partition, path);
    if (const auto it = d.pending.find(key); it != d.pending.end() && it->second.type == OpType::mkdir &&
                                             !d.will_exist_on_disk(it->second)) {
        ++d.stats.coalesced_ops;
        d.journal("C " + std::to_string(it->second.seq));
        d.pending.erase(it);
        return WriteStatus::ok;
    }
    Op op;
    op.type = OpType::rmdir;
    op.seq = d.next_seq++;
    op.partition = std::string(partition);
    op.path = path;
    op.existed_on_disk = true;
    d.journal("R " + std::to_string(op.seq) + " " + op.partition + "\t" + op.path);
    d.enqueue(std::move(op));
    return WriteStatus::ok;
}

WriteStatus PfsWriteBack::rename(std::string_view partition, std::string_view raw_from,
                                 std::string_view raw_to, bool replace_existing)
{
    auto& d = *impl_;
    const auto from = normalize(raw_from);
    const auto to = normalize(raw_to);
    if (from == "/" || to == "/") return WriteStatus::invalid;
    if (from == to) return WriteStatus::ok;
    // PFS names are case-sensitive while Windows is not: a case-only rename
    // would need two names that collide in the overlay. Refuse it explicitly.
    if (upper_ascii(from) == upper_ascii(to)) return WriteStatus::invalid;
    Kind kind;
    {
        std::unique_lock lock(d.mutex);
        if (const auto admitted = d.admit_mutation(partition); admitted != WriteStatus::ok) return admitted;
        kind = d.effective_kind(partition, from);
        if (kind == Kind::none) return WriteStatus::not_found;
        if (upper_ascii(to).rfind(upper_ascii(from) + "/", 0) == 0) return WriteStatus::invalid;
    }

    if (kind == Kind::file) {
        // Copy-on-write move: stage the new name, then retire the old one.
        // Both become ordinary queued ops, so a crash leaves at worst a copy.
        Handle handle{};
        if (const auto status = open_write(partition, to, true, !replace_existing, handle); status != WriteStatus::ok) {
            return status;
        }
        {
            std::unique_lock lock(d.mutex);
            auto& h = d.handles.at(handle);
            if (!d.copy_into(partition, from, h.spool, h.size)) {
                lock.unlock();
                (void)close(handle, true);
                return WriteStatus::io_error;
            }
            h.dirty = true;
        }
        if (const auto status = close(handle); status != WriteStatus::ok) return status;
        return remove_file(partition, from);
    }

    // Directory: recursive copy-on-write move, bounded by the pending budget.
    if (const auto status = make_directory(partition, to); status != WriteStatus::ok) return status;
    for (const auto& child : pending_children(partition, from)) {
        const auto name = leaf_of(child.path);
        if (const auto status = rename(partition, from + "/" + name, to + "/" + name, replace_existing);
            status != WriteStatus::ok) {
            return status;
        }
    }
    return remove_directory(partition, from);
}

bool PfsWriteBack::flush(std::chrono::milliseconds timeout)
{
    auto& d = *impl_;
    std::unique_lock lock(d.mutex);
    const auto failed_before = d.stats.failed_ops;
    d.flush_requested = true;
    d.cv.notify_all();
    const bool drained = d.done_cv.wait_for(lock, timeout, [&d] {
        return (d.pending.empty() && d.in_flight.empty()) || d.read_only;
    });
    return drained && !d.read_only && d.stats.failed_ops == failed_before;
}

std::optional<PendingNode> PfsWriteBack::pending(std::string_view partition, std::string_view raw_path) const
{
    auto& d = *impl_;
    const auto path = normalize(raw_path);
    std::unique_lock lock(d.mutex);
    for (const auto& [id, h] : d.handles) {
        if (h.partition == partition && upper_ascii(h.path) == upper_ascii(path)) {
            return PendingNode{PendingNode::Kind::file, h.path, h.size};
        }
    }
    const auto it = d.pending.find(make_key(partition, path));
    if (it == d.pending.end()) return std::nullopt;
    switch (it->second.type) {
    case OpType::write: return PendingNode{PendingNode::Kind::file, it->second.path, it->second.size};
    case OpType::mkdir: return PendingNode{PendingNode::Kind::directory, it->second.path, 0};
    case OpType::remove:
    case OpType::rmdir:
    default: return PendingNode{PendingNode::Kind::deleted, it->second.path, 0};
    }
}

std::vector<PendingNode> PfsWriteBack::pending_children(std::string_view partition,
                                                        std::string_view raw_directory) const
{
    // Merged view: on-disk children, minus pending deletes, plus pending
    // files/directories and open handles.
    auto& d = *impl_;
    const auto directory = normalize(raw_directory);
    std::map<std::string, PendingNode> merged;
    const auto browse = d.session.browse(partition, directory);
    if (browse.ok) {
        for (const auto& entry : browse.entries) {
            if (entry.name == "." || entry.name == "..") continue;
            const auto child = (directory == "/" ? "/" : directory + "/") + entry.name;
            merged[upper_ascii(entry.name)] = PendingNode{
                entry.is_directory() ? PendingNode::Kind::directory : PendingNode::Kind::file, child, entry.size};
        }
    }
    std::unique_lock lock(d.mutex);
    const auto prefix = upper_ascii(directory == "/" ? "/" : directory + "/");
    const auto apply = [&](const std::string& path, PendingNode node) {
        const auto upper = upper_ascii(path);
        if (upper.rfind(prefix, 0) != 0 || upper.find('/', prefix.size()) != std::string::npos ||
            upper.size() == prefix.size()) {
            return;
        }
        const auto name = upper.substr(prefix.size());
        if (node.kind == PendingNode::Kind::deleted) merged.erase(name);
        else merged[name] = std::move(node);
    };
    for (const auto& [key, op] : d.pending) {
        if (op.partition != partition) continue;
        PendingNode node{op.type == OpType::write ? PendingNode::Kind::file
                         : op.type == OpType::mkdir ? PendingNode::Kind::directory
                                                    : PendingNode::Kind::deleted,
                         op.path, op.size};
        apply(op.path, std::move(node));
    }
    for (const auto& [id, h] : d.handles) {
        if (h.partition == partition) apply(h.path, PendingNode{PendingNode::Kind::file, h.path, h.size});
    }
    std::vector<PendingNode> out;
    out.reserve(merged.size());
    for (auto& [name, node] : merged) out.push_back(std::move(node));
    return out;
}

bool PfsWriteBack::read_pending(std::string_view partition, std::string_view raw_path, std::uint64_t offset,
                                std::span<std::byte> out, std::size_t& bytes_read) const
{
    auto& d = *impl_;
    const auto path = normalize(raw_path);
    std::unique_lock lock(d.mutex);
    bytes_read = 0;
    std::filesystem::path spool;
    std::uint64_t size = 0;
    for (const auto& [id, h] : d.handles) {
        if (h.partition == partition && upper_ascii(h.path) == upper_ascii(path)) {
            spool = h.spool;
            size = h.size;
        }
    }
    if (spool.empty()) {
        const auto it = d.pending.find(make_key(partition, path));
        if (it == d.pending.end() || it->second.type != OpType::write) return false;
        spool = it->second.spool;
        size = it->second.size;
    }
    if (offset >= size) return true;
    std::ifstream in(spool, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(reinterpret_cast<char*>(out.data()),
            static_cast<std::streamsize>(std::min<std::uint64_t>(out.size(), size - offset)));
    bytes_read = static_cast<std::size_t>(in.gcount());
    return true;
}

std::optional<std::uint64_t> PfsWriteBack::available_bytes(std::string_view partition)
{
    auto& d = *impl_;
    {
        std::unique_lock lock(d.mutex);
        if (const auto it = d.free_cache.find(std::string(partition)); it != d.free_cache.end()) {
            const auto used = d.options.free_space_reserve_bytes + d.pending_write_bytes();
            return it->second > used ? it->second - used : 0;
        }
    }
    // First query: measure once through a short write-capability session.
    std::string error;
    auto device = d.factory ? d.factory(error) : nullptr;
    const auto* p = d.session.find_partition(partition);
    if (!device || p == nullptr) return std::nullopt;
    pfs::ImageWriter writer(*device, *p);
    const auto free = writer.valid() ? writer.free_bytes() : std::nullopt;
    if (!free) return std::nullopt;
    std::unique_lock lock(d.mutex);
    d.free_cache[std::string(partition)] = *free;
    d.zone_size = writer.zone_bytes();
    const auto used = d.options.free_space_reserve_bytes + d.pending_write_bytes();
    return *free > used ? *free - used : 0;
}

WriteBackStats PfsWriteBack::stats() const
{
    std::unique_lock lock(impl_->mutex);
    auto s = impl_->stats;
    s.pending_ops = impl_->pending.size();
    s.pending_bytes = impl_->pending_write_bytes();
    s.open_handles = impl_->handles.size();
    s.read_only = impl_->read_only;
    return s;
}

bool PfsWriteBack::read_only() const
{
    std::unique_lock lock(impl_->mutex);
    return impl_->read_only;
}

void PfsWriteBack::reset_breaker()
{
    std::unique_lock lock(impl_->mutex);
    impl_->read_only = false;
    impl_->consecutive_failures = 0;
}

std::vector<JournalOrphan> PfsWriteBack::scan_journal(const std::filesystem::path& spool_directory)
{
    std::ifstream in(spool_directory / "journal.log");
    std::map<std::uint64_t, JournalOrphan> writes;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string tag;
        std::uint64_t seq = 0;
        if (!(fields >> tag >> seq)) continue;
        if (tag == "C" || tag == "F") {
            writes.erase(seq);
        } else if (tag == "W") {
            JournalOrphan orphan;
            orphan.sequence = seq;
            std::string spool;
            fields >> orphan.size >> spool;
            fields.get();
            std::string rest;
            std::getline(fields, rest);
            const auto tab = rest.find('\t');
            if (tab == std::string::npos) continue;
            orphan.partition = rest.substr(0, tab);
            orphan.path = rest.substr(tab + 1);
            orphan.spool_file = spool_directory / spool;
            writes[seq] = std::move(orphan);
        }
    }
    std::vector<JournalOrphan> out;
    for (auto& [seq, orphan] : writes) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(orphan.spool_file, ec) &&
            std::filesystem::file_size(orphan.spool_file, ec) == orphan.size) {
            out.push_back(std::move(orphan));
        }
    }
    return out;
}

std::size_t PfsWriteBack::replay_orphans(const std::vector<JournalOrphan>& orphans)
{
    auto& d = *impl_;
    std::unique_lock lock(d.mutex);
    std::size_t queued = 0;
    for (const auto& orphan : orphans) {
        if (d.read_only || !partition_writable(orphan.partition)) continue;
        Op op;
        op.type = OpType::write;
        op.seq = d.next_seq++;
        op.partition = orphan.partition;
        op.path = normalize(orphan.path);
        op.size = orphan.size;
        op.spool = d.options.spool_directory / ("w" + std::to_string(op.seq) + ".bin");
        std::error_code ec;
        std::filesystem::rename(orphan.spool_file, op.spool, ec);
        if (ec) continue;
        op.existed_on_disk = d.disk_kind(op.partition, op.path) == Kind::file;
        d.journal("C " + std::to_string(orphan.sequence)); // retire the old intent
        d.journal("W " + std::to_string(op.seq) + " " + std::to_string(op.size) + " " +
                  op.spool.filename().string() + " " + op.partition + "\t" + op.path);
        d.enqueue(std::move(op));
        ++queued;
    }
    return queued;
}

} // namespace ps2hdd
