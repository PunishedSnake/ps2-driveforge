#pragma once

#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ps2hdd {

// DriveForge write-back engine for PFS ("the driver layer").
//
// Legacy PS2 PC tools map every application write() straight onto the PS2
// disk as a PFS metadata transaction. Explorer copies issue thousands of
// small writes, so those tools saturate slow IDE/USB bridges with seeks and
// flushes until Windows times the device out. DriveForge never lets the
// caller's I/O pattern reach the disk:
//
//   open/write/truncate  -> host spool file per handle (no disk I/O)
//   close                -> coalesced pending op (last write wins, write+delete
//                           of a new file vanishes, mkdir+rmdir vanishes)
//   committer thread     -> debounced batches, ordered by intent sequence,
//                           one native pfs::ImageWriter session per partition
//                           per batch, whole-file copy-on-write per file,
//                           pause between batches, caches invalidated after
//   intent journal       -> spool/journal.log records W(rite)/D(elete)/M/R
//                           intents and C(ommit) marks; uncommitted spool data
//                           survives a crash and can be replayed explicitly.
//
// Failsafes: bounded pending bytes/files (backpressure -> busy), a free-space
// reserve the PFS is never filled past, a per-file size cap for whole-file
// CoW, protected system partitions, and a circuit breaker that turns the
// engine read-only after consecutive failed batches instead of retrying a
// struggling disk into the ground.

struct WriteBackOptions {
    std::filesystem::path spool_directory;
    std::uint64_t max_pending_bytes{512ULL * 1024ULL * 1024ULL};
    std::size_t max_pending_ops{4096};
    std::uint64_t max_file_bytes{1024ULL * 1024ULL * 1024ULL};
    std::uint64_t free_space_reserve_bytes{64ULL * 1024ULL * 1024ULL};
    std::chrono::milliseconds commit_delay{750};
    std::chrono::milliseconds batch_pause{40};
    std::size_t max_ops_per_batch{256};
    std::size_t breaker_failed_batches{3};
    // Partitions that are writable. Empty = every PFS partition except the
    // protected system set below.
    std::set<std::string> writable_partitions;
    // Never writable unless explicitly listed in writable_partitions.
    std::set<std::string> protected_partitions{"__system", "__sysconf", "__net", "__mbr"};
};

// Opens a fresh write capability for one batch. Image sources return a
// WritableFileBlockDevice; physical sources return a short-lived guarded lease.
using WriterFactory =
    std::function<std::unique_ptr<WritableBlockDevice>(std::string& error)>;

enum class WriteStatus {
    ok,
    not_found,
    exists,
    is_directory,
    not_directory,
    not_empty,
    read_only,
    protected_partition,
    busy,
    no_space,
    too_large,
    invalid,
    io_error,
};

[[nodiscard]] const char* write_status_name(WriteStatus status) noexcept;

struct WriteBackStats {
    std::size_t pending_ops{};
    std::uint64_t pending_bytes{};
    std::size_t open_handles{};
    std::size_t committed_ops{};
    std::size_t committed_batches{};
    std::size_t failed_ops{};
    std::size_t failed_batches{};
    std::size_t coalesced_ops{};
    std::size_t application_writes{};
    std::size_t disk_write_sessions{};
    bool read_only{};
    std::string last_error;
};

// Overlay state a mount view merges with the on-disk PFS tree.
struct PendingNode {
    enum class Kind { file, directory, deleted } kind{Kind::file};
    std::string path; // display-case PFS path
    std::uint64_t size{};
};

struct JournalOrphan {
    std::uint64_t sequence{};
    std::string partition;
    std::string path;
    std::uint64_t size{};
    std::filesystem::path spool_file;
};

class PfsWriteBack {
public:
    using Handle = std::uint64_t;

    PfsWriteBack(DriveSession& session, WriterFactory factory, WriteBackOptions options);
    ~PfsWriteBack();
    PfsWriteBack(const PfsWriteBack&) = delete;
    PfsWriteBack& operator=(const PfsWriteBack&) = delete;

    [[nodiscard]] bool partition_writable(std::string_view partition) const;

    // Handles. open_write seeds the spool from the current content (pending
    // or on-disk) unless truncate is set. create_new fails if the path exists.
    [[nodiscard]] WriteStatus open_write(std::string_view partition, std::string_view path,
                                         bool truncate, bool create_new, Handle& handle);
    [[nodiscard]] WriteStatus write(Handle handle, std::uint64_t offset,
                                    std::span<const std::byte> data);
    [[nodiscard]] WriteStatus set_size(Handle handle, std::uint64_t size);
    [[nodiscard]] WriteStatus read(Handle handle, std::uint64_t offset, std::span<std::byte> out,
                                   std::size_t& bytes_read);
    [[nodiscard]] std::optional<std::uint64_t> handle_size(Handle handle) const;
    // Queues the commit. discard=true drops the handle without committing
    // (used for delete-on-close of a freshly created file).
    WriteStatus close(Handle handle, bool discard = false);

    [[nodiscard]] WriteStatus remove_file(std::string_view partition, std::string_view path);
    [[nodiscard]] WriteStatus make_directory(std::string_view partition, std::string_view path);
    [[nodiscard]] WriteStatus remove_directory(std::string_view partition, std::string_view path);
    [[nodiscard]] WriteStatus rename(std::string_view partition, std::string_view from,
                                     std::string_view to, bool replace_existing);

    // Commit everything queued now; returns false on timeout or failure.
    bool flush(std::chrono::milliseconds timeout = std::chrono::seconds(60));

    // Overlay queries (case-insensitive, like Windows).
    [[nodiscard]] std::optional<PendingNode> pending(std::string_view partition,
                                                     std::string_view path) const;
    [[nodiscard]] std::vector<PendingNode> pending_children(std::string_view partition,
                                                            std::string_view directory) const;
    [[nodiscard]] bool read_pending(std::string_view partition, std::string_view path,
                                    std::uint64_t offset, std::span<std::byte> out,
                                    std::size_t& bytes_read) const;

    // Free bytes as the application should see them: PFS free space minus the
    // reserve and minus queued-but-uncommitted growth.
    [[nodiscard]] std::optional<std::uint64_t> available_bytes(std::string_view partition);

    [[nodiscard]] WriteBackStats stats() const;
    [[nodiscard]] bool read_only() const;
    // Operator override after the cause of a breaker trip was fixed.
    void reset_breaker();

    // Crash recovery: spool data whose W intent has no C mark.
    [[nodiscard]] static std::vector<JournalOrphan> scan_journal(
        const std::filesystem::path& spool_directory);
    // Re-queues orphans (as regular pending writes) and returns how many.
    std::size_t replay_orphans(const std::vector<JournalOrphan>& orphans);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ps2hdd
