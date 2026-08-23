#include "ps2hdd/mutation_journal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ps2hdd {
namespace {

constexpr std::array<char, 8> kMagic{'P', 'S', '2', 'D', 'F', 'R', 'C', '1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaxRanges = 4096;
constexpr std::uint64_t kMaxRangeBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxCapturedBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxJournalFileBytes = 132ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaxLabelBytes = 4096;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct LoadedJournal {
    MutationJournalState state{MutationJournalState::prepared};
    std::uint64_t device_size{};
    std::vector<StagedWrite> writes;
    std::uint64_t captured_bytes{};
};

void append_u32(std::vector<std::byte>& out, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::byte>& out, std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xffULL));
    }
}

bool read_u32(std::span<const std::byte> bytes, std::size_t& cursor,
              std::uint32_t& value)
{
    if (cursor > bytes.size() || 4U > bytes.size() - cursor) {
        return false;
    }
    value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<unsigned char>(bytes[cursor + index]))
                 << (index * 8U);
    }
    cursor += 4;
    return true;
}

bool read_u64(std::span<const std::byte> bytes, std::size_t& cursor,
              std::uint64_t& value)
{
    if (cursor > bytes.size() || 8U > bytes.size() - cursor) {
        return false;
    }
    value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned char>(bytes[cursor + index]))
                 << (index * 8U);
    }
    cursor += 8;
    return true;
}

std::uint64_t fnv1a(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t hash = kFnvOffset;
    for (const auto value : bytes) {
        hash ^= std::to_integer<unsigned char>(value);
        hash *= kFnvPrime;
    }
    return hash;
}

bool range_fits(std::uint64_t offset, std::uint64_t bytes,
                std::uint64_t size) noexcept
{
    return offset <= size && bytes <= size - offset;
}

bool validate_writes(std::uint64_t device_size,
                     std::span<const StagedWrite> writes,
                     std::uint64_t& captured,
                     std::string& error)
{
    captured = 0;
    if (writes.empty() || writes.size() > kMaxRanges) {
        error = "Mutation journal requires 1..4096 staged metadata ranges";
        return false;
    }

    for (std::size_t index = 0; index < writes.size(); ++index) {
        const auto& write = writes[index];
        if (write.before.empty() || write.before.size() != write.after.size()) {
            error = "Mutation journal range has inconsistent before/after byte counts";
            return false;
        }
        if (write.before.size() > kMaxRangeBytes ||
            write.offset % kMutationSectorSize != 0 ||
            write.before.size() % kMutationSectorSize != 0 ||
            !range_fits(write.offset, write.before.size(), device_size)) {
            error = "Mutation journal range is outside the bounded sector-aligned metadata surface";
            return false;
        }
        if (write.label.size() > kMaxLabelBytes) {
            error = "Mutation journal range label exceeds the format limit";
            return false;
        }
        if (captured > kMaxCapturedBytes - write.before.size()) {
            error = "Mutation journal exceeds the 64 MiB before-image limit";
            return false;
        }
        captured += write.before.size();

        const auto end = write.offset + write.before.size();
        for (std::size_t prior = 0; prior < index; ++prior) {
            const auto& other = writes[prior];
            const auto other_end = other.offset + other.before.size();
            if (write.offset < other_end && other.offset < end) {
                error = "Mutation journal contains overlapping ranges";
                return false;
            }
        }
    }
    return true;
}

std::vector<std::byte> serialize(const LoadedJournal& journal,
                                 std::string& error)
{
    std::uint64_t captured = 0;
    if (!validate_writes(journal.device_size, journal.writes, captured, error)) {
        return {};
    }
    if (journal.state != MutationJournalState::prepared &&
        journal.state != MutationJournalState::committed &&
        journal.state != MutationJournalState::restored) {
        error = "Mutation journal has an unsupported lifecycle state";
        return {};
    }

    std::uint64_t estimated = 40;
    for (const auto& write : journal.writes) {
        const auto record = 24ULL + 2ULL * write.before.size() + write.label.size();
        if (estimated > kMaxJournalFileBytes - record) {
            error = "Mutation journal serialized size exceeds the bounded file limit";
            return {};
        }
        estimated += record;
    }

    std::vector<std::byte> bytes;
    bytes.reserve(static_cast<std::size_t>(estimated));
    for (const char ch : kMagic) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    append_u32(bytes, kVersion);
    append_u32(bytes, static_cast<std::uint32_t>(journal.state));
    append_u64(bytes, journal.device_size);
    append_u32(bytes, static_cast<std::uint32_t>(journal.writes.size()));
    append_u32(bytes, 0);

    for (const auto& write : journal.writes) {
        append_u64(bytes, write.offset);
        append_u64(bytes, write.before.size());
        append_u32(bytes, static_cast<std::uint32_t>(write.label.size()));
        append_u32(bytes, 0);
        bytes.insert(bytes.end(), write.before.begin(), write.before.end());
        bytes.insert(bytes.end(), write.after.begin(), write.after.end());
        for (const char ch : write.label) {
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
        }
    }
    append_u64(bytes, fnv1a(bytes));
    return bytes;
}

bool durable_replace(const std::filesystem::path& path,
                     std::span<const std::byte> bytes,
                     std::string& error)
{
    if (path.empty()) {
        error = "Mutation journal path is empty";
        return false;
    }

    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "Could not create mutation journal parent directory: " + ec.message();
            return false;
        }
    }

    auto temporary = path;
    temporary += ".tmp";

#if defined(_WIN32)
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Could not create mutation journal temporary file";
        return false;
    }

    bool write_ok = true;
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - written,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD done = 0;
        if (!WriteFile(file,
                       bytes.data() + static_cast<std::ptrdiff_t>(written),
                       amount, &done, nullptr) || done != amount) {
            write_ok = false;
            break;
        }
        written += done;
    }
    const bool flush_ok = write_ok && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!write_ok || !flush_ok) {
        DeleteFileW(temporary.c_str());
        error = "Could not durably write mutation journal temporary file";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = "Could not atomically publish mutation journal";
        return false;
    }
#else
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        error = "Could not create mutation journal temporary file: " +
                std::string(std::strerror(errno));
        return false;
    }

    bool write_ok = true;
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto* data = reinterpret_cast<const char*>(bytes.data()) +
                           static_cast<std::ptrdiff_t>(written);
        const auto result = ::write(fd, data, bytes.size() - written);
        if (result <= 0) {
            write_ok = false;
            break;
        }
        written += static_cast<std::size_t>(result);
    }
    const bool flush_ok = write_ok && ::fsync(fd) == 0;
    const bool close_ok = ::close(fd) == 0;
    if (!write_ok || !flush_ok || !close_ok) {
        ::unlink(temporary.c_str());
        error = "Could not durably write mutation journal temporary file: " +
                std::string(std::strerror(errno));
        return false;
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        error = "Could not atomically publish mutation journal: " +
                std::string(std::strerror(errno));
        return false;
    }

    const auto directory = parent.empty() ? std::filesystem::path{"."} : parent;
#if defined(O_DIRECTORY)
    const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
#else
    const int dir_fd = ::open(directory.c_str(), O_RDONLY);
#endif
    if (dir_fd >= 0) {
        const bool dir_ok = ::fsync(dir_fd) == 0;
        ::close(dir_fd);
        if (!dir_ok) {
            error = "Mutation journal was renamed but its directory flush failed";
            return false;
        }
    }
#endif
    return true;
}

bool load_journal(const std::filesystem::path& path,
                  LoadedJournal& journal,
                  std::string& error)
{
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size < 40 || file_size > kMaxJournalFileBytes) {
        error = "Mutation journal is missing or outside the bounded file-size range";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open mutation journal";
        return false;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        error = "Could not read complete mutation journal";
        return false;
    }

    const auto body = std::span<const std::byte>(bytes).first(bytes.size() - 8U);
    std::size_t checksum_cursor = bytes.size() - 8U;
    std::uint64_t stored_hash = 0;
    if (!read_u64(bytes, checksum_cursor, stored_hash) ||
        stored_hash != fnv1a(body)) {
        error = "Mutation journal checksum does not match its contents";
        return false;
    }

    for (std::size_t index = 0; index < kMagic.size(); ++index) {
        if (std::to_integer<unsigned char>(bytes[index]) !=
            static_cast<unsigned char>(kMagic[index])) {
            error = "Mutation journal magic is invalid";
            return false;
        }
    }

    std::size_t cursor = kMagic.size();
    std::uint32_t version = 0;
    std::uint32_t raw_state = 0;
    std::uint32_t range_count = 0;
    std::uint32_t reserved = 0;
    if (!read_u32(body, cursor, version) || version != kVersion ||
        !read_u32(body, cursor, raw_state) ||
        !read_u64(body, cursor, journal.device_size) ||
        !read_u32(body, cursor, range_count) ||
        !read_u32(body, cursor, reserved) || reserved != 0 ||
        range_count == 0 || range_count > kMaxRanges) {
        error = "Mutation journal header is invalid or unsupported";
        return false;
    }
    if (raw_state < static_cast<std::uint32_t>(MutationJournalState::prepared) ||
        raw_state > static_cast<std::uint32_t>(MutationJournalState::restored)) {
        error = "Mutation journal lifecycle state is invalid";
        return false;
    }
    journal.state = static_cast<MutationJournalState>(raw_state);
    journal.writes.clear();
    journal.writes.reserve(range_count);

    for (std::uint32_t index = 0; index < range_count; ++index) {
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        std::uint32_t label_length = 0;
        std::uint32_t record_reserved = 0;
        if (!read_u64(body, cursor, offset) ||
            !read_u64(body, cursor, length) ||
            !read_u32(body, cursor, label_length) ||
            !read_u32(body, cursor, record_reserved) || record_reserved != 0 ||
            length == 0 || length > kMaxRangeBytes ||
            label_length > kMaxLabelBytes ||
            length > std::numeric_limits<std::size_t>::max()) {
            error = "Mutation journal range header is malformed";
            return false;
        }
        const auto needed = 2ULL * length + label_length;
        if (cursor > body.size() || needed > body.size() - cursor) {
            error = "Mutation journal range payload is truncated";
            return false;
        }

        StagedWrite write;
        write.offset = offset;
        const auto size = static_cast<std::size_t>(length);
        write.before.assign(body.begin() + static_cast<std::ptrdiff_t>(cursor),
                            body.begin() + static_cast<std::ptrdiff_t>(cursor + size));
        cursor += size;
        write.after.assign(body.begin() + static_cast<std::ptrdiff_t>(cursor),
                           body.begin() + static_cast<std::ptrdiff_t>(cursor + size));
        cursor += size;
        write.label.assign(
            reinterpret_cast<const char*>(body.data() + cursor), label_length);
        cursor += label_length;
        journal.writes.emplace_back(std::move(write));
    }
    if (cursor != body.size()) {
        error = "Mutation journal contains unexpected trailing record bytes";
        return false;
    }

    std::uint64_t captured = 0;
    if (!validate_writes(journal.device_size, journal.writes, captured, error)) {
        return false;
    }
    journal.captured_bytes = captured;
    return true;
}

MutationJournalInspection inspect_loaded(const LoadedJournal& journal,
                                         WritableBlockDevice& device)
{
    MutationJournalInspection result;
    result.journal_state = journal.state;
    result.ranges = journal.writes.size();
    result.captured_bytes = journal.captured_bytes;
    if (device.size_bytes() != journal.device_size) {
        result.error = "Mutation journal target size does not match the current device";
        result.device_state = MutationDeviceState::foreign_or_corrupt;
        result.ok = true;
        return result;
    }

    for (const auto& write : journal.writes) {
        std::vector<std::byte> current(write.before.size());
        if (!device.read(write.offset, current)) {
            result.error = "Could not read a mutation-journal protected range";
            return result;
        }
        const bool before = current == write.before;
        const bool after = current == write.after;
        if (before && after) {
            ++result.unchanged_ranges;
        } else if (before) {
            ++result.before_ranges;
        } else if (after) {
            ++result.after_ranges;
        } else {
            result.device_state = MutationDeviceState::foreign_or_corrupt;
            result.ok = true;
            return result;
        }
    }

    if (result.before_ranges != 0 && result.after_ranges != 0) {
        result.device_state = MutationDeviceState::mixed;
    } else if (result.after_ranges != 0) {
        result.device_state = MutationDeviceState::all_after;
    } else {
        result.device_state = MutationDeviceState::all_before;
    }
    result.ok = true;
    return result;
}

MutationJournalResult rewrite_state(const std::filesystem::path& path,
                                    MutationJournalState state)
{
    MutationJournalResult result;
    LoadedJournal journal;
    if (!load_journal(path, journal, result.error)) {
        return result;
    }
    journal.state = state;
    std::string serialize_error;
    auto bytes = serialize(journal, serialize_error);
    if (bytes.empty()) {
        result.error = std::move(serialize_error);
        return result;
    }
    if (!durable_replace(path, bytes, result.error)) {
        return result;
    }
    result.ok = true;
    result.ranges = journal.writes.size();
    result.captured_bytes = journal.captured_bytes;
    return result;
}

} // namespace

MutationJournalResult create_mutation_journal(
    const std::filesystem::path& path,
    const WritableBlockDevice& device,
    std::span<const StagedWrite> writes)
{
    MutationJournalResult result;
    LoadedJournal journal;
    journal.state = MutationJournalState::prepared;
    journal.device_size = device.size_bytes();
    journal.writes.assign(writes.begin(), writes.end());

    std::string serialize_error;
    auto bytes = serialize(journal, serialize_error);
    if (bytes.empty()) {
        result.error = std::move(serialize_error);
        return result;
    }
    if (!durable_replace(path, bytes, result.error)) {
        return result;
    }

    std::uint64_t captured = 0;
    std::string ignored;
    (void)validate_writes(journal.device_size, journal.writes, captured, ignored);
    result.ok = true;
    result.ranges = journal.writes.size();
    result.captured_bytes = captured;
    return result;
}

MutationJournalInspection inspect_mutation_journal(
    const std::filesystem::path& path,
    WritableBlockDevice& device)
{
    LoadedJournal journal;
    MutationJournalInspection result;
    if (!load_journal(path, journal, result.error)) {
        return result;
    }
    return inspect_loaded(journal, device);
}

MutationJournalResult mark_mutation_journal_committed(
    const std::filesystem::path& path)
{
    LoadedJournal journal;
    MutationJournalResult result;
    if (!load_journal(path, journal, result.error)) {
        return result;
    }
    if (journal.state == MutationJournalState::committed) {
        result.ok = true;
        result.ranges = journal.writes.size();
        result.captured_bytes = journal.captured_bytes;
        return result;
    }
    if (journal.state != MutationJournalState::prepared) {
        result.error = "Only a PREPARED mutation journal can be marked COMMITTED";
        return result;
    }
    return rewrite_state(path, MutationJournalState::committed);
}

MutationJournalRestoreResult restore_prepared_mutation_journal(
    const std::filesystem::path& path,
    WritableBlockDevice& device)
{
    MutationJournalRestoreResult result;
    LoadedJournal journal;
    if (!load_journal(path, journal, result.error)) {
        return result;
    }
    result.inspection = inspect_loaded(journal, device);
    if (!result.inspection.ok) {
        result.error = result.inspection.error;
        return result;
    }
    if (journal.state != MutationJournalState::prepared) {
        result.error = "Automatic rollback only restores a PREPARED mutation journal";
        return result;
    }
    if (result.inspection.device_state == MutationDeviceState::foreign_or_corrupt) {
        result.error = "Mutation target contains bytes matching neither journal before nor after images";
        return result;
    }

    for (auto it = journal.writes.rbegin(); it != journal.writes.rend(); ++it) {
        ++result.writes_attempted;
        if (!device.write(it->offset, it->before)) {
            result.error = "Could not restore a mutation journal before-image";
            return result;
        }
    }
    if (!device.flush()) {
        result.error = "Could not flush restored mutation journal ranges";
        return result;
    }
    for (const auto& write : journal.writes) {
        std::vector<std::byte> readback(write.before.size());
        if (!device.read(write.offset, readback) || readback != write.before) {
            result.error = "Mutation journal before-image verification failed";
            return result;
        }
    }

    const auto marked = rewrite_state(path, MutationJournalState::restored);
    if (!marked.ok) {
        result.error = "Target before-images were restored, but mutation journal state could not be marked RESTORED: " +
                       marked.error;
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace ps2hdd
