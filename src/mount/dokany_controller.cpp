#include "ps2hdd/dokany_mount.hpp"

#ifdef _WIN32

#include "ps2hdd/apa.hpp"
#include "ps2hdd/darkness_policy.hpp"
#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/mount_view.hpp"
#include "ps2hdd/pfs_write_back.hpp"
#include "ps2hdd/writable_mount_view.hpp"
#include "dokany_open_policy.hpp"

#if __has_include(<dokan/dokan.h>)
#include <dokan/dokan.h>
#else
#include <dokan.h>
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ps2hdd {
namespace {

struct MountRuntime {
    MountRuntime(std::unique_ptr<BlockDevice> source, std::wstring point, bool debug_enabled,
                 std::atomic_bool* mounted_flag)
        : session(std::move(source)), view(session), mount_point(std::move(point)),
          debug(debug_enabled), mounted(mounted_flag)
    {
    }

    DriveSession session;
    ReadOnlyMountView view;
    std::wstring mount_point;
    bool debug{};
    std::atomic_bool* mounted{};
    // Frieren write-back. Declared after session so they are destroyed first:
    // the engine's destructor drains queued commits through the session.
    std::unique_ptr<PfsWriteBack> engine;
    std::unique_ptr<WritableMountView> writable;

    [[nodiscard]] MountLookupResult lookup(std::string_view path)
    {
        return writable ? writable->lookup(path) : view.lookup(path);
    }
    [[nodiscard]] MountListResult list(std::string_view path)
    {
        return writable ? writable->list_directory(path) : view.list_directory(path);
    }
    [[nodiscard]] MountReadResult read(std::string_view path, std::uint64_t offset, std::span<std::byte> out)
    {
        return writable ? writable->read_file(path, offset, out) : view.read_file(path, offset, out);
    }
};

// Per-open state carried in DOKAN_FILE_INFO::Context.
struct OpenState {
    std::string partition;
    std::string pfs_path;
    bool directory{};
    bool has_handle{};
    PfsWriteBack::Handle handle{};
};

OpenState* open_state(PDOKAN_FILE_INFO info)
{
    return info ? reinterpret_cast<OpenState*>(info->Context) : nullptr;
}

NTSTATUS map_write_status(WriteStatus status)
{
    switch (status) {
    case WriteStatus::ok: return STATUS_SUCCESS;
    case WriteStatus::not_found: return STATUS_OBJECT_NAME_NOT_FOUND;
    case WriteStatus::exists: return STATUS_OBJECT_NAME_COLLISION;
    case WriteStatus::is_directory: return STATUS_FILE_IS_A_DIRECTORY;
    case WriteStatus::not_directory: return STATUS_NOT_A_DIRECTORY;
    case WriteStatus::not_empty: return STATUS_DIRECTORY_NOT_EMPTY;
    case WriteStatus::read_only:
    case WriteStatus::protected_partition: return STATUS_MEDIA_WRITE_PROTECTED;
    case WriteStatus::busy: return STATUS_DEVICE_BUSY;
    case WriteStatus::no_space: return STATUS_DISK_FULL;
    case WriteStatus::too_large: return STATUS_FILE_TOO_LARGE;
    case WriteStatus::invalid: return STATUS_NOT_SUPPORTED;
    case WriteStatus::io_error:
    default: return STATUS_IO_DEVICE_ERROR;
    }
}

bool wants_data_write(ACCESS_MASK access)
{
    return (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | GENERIC_ALL)) != 0;
}

// Resolves the PFS target of an open for writable mounts; nullptr when the
// open is outside a writable PFS tree (read-only rules then apply).
std::optional<WritableMountView::Target> writable_target(MountRuntime& runtime, const std::string& path)
{
    if (!runtime.writable) return std::nullopt;
    auto target = runtime.writable->resolve(path);
    if (!target.ok || target.partition.empty() || !target.writable) return std::nullopt;
    return target;
}

NTSTATUS ensure_handle(MountRuntime& runtime, OpenState& state, bool truncate)
{
    if (state.has_handle) return STATUS_SUCCESS;
    const auto status = runtime.engine->open_write(state.partition, state.pfs_path, truncate, false, state.handle);
    if (status != WriteStatus::ok) return map_write_status(status);
    state.has_handle = true;
    return STATUS_SUCCESS;
}

MountRuntime* context(PDOKAN_FILE_INFO info)
{
    if (!info || !info->DokanOptions) {
        return nullptr;
    }
    return reinterpret_cast<MountRuntime*>(info->DokanOptions->GlobalContext);
}

std::optional<std::string> wide_to_utf8(LPCWSTR value)
{
    if (!value) {
        return std::string{};
    }
    const int length = static_cast<int>(std::wcslen(value));
    if (length == 0) {
        return std::string{};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length,
                            result.data(), bytes, nullptr, nullptr) != bytes) {
        return std::nullopt;
    }
    return result;
}

std::wstring utf8_to_wide(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (chars > 0) {
        std::wstring result(static_cast<std::size_t>(chars), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                static_cast<int>(value.size()), result.data(), chars) == chars) {
            return result;
        }
    }

    // PFS names are byte strings. Preserve otherwise undecodable legacy bytes in
    // the low Unicode range rather than making the whole mount unavailable.
    std::wstring fallback;
    fallback.reserve(value.size());
    for (const unsigned char byte : value) {
        fallback.push_back(static_cast<wchar_t>(byte));
    }
    return fallback;
}

DWORD file_attributes(bool directory, bool writable = false)
{
    if (writable) return directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    return FILE_ATTRIBUTE_READONLY | (directory ? FILE_ATTRIBUTE_DIRECTORY : 0U);
}

bool has_write_access(ACCESS_MASK access)
{
    constexpr ACCESS_MASK write_mask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                                       FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER |
                                       GENERIC_WRITE | GENERIC_ALL;
    return (access & write_mask) != 0;
}

NTSTATUS map_open_status(dokany_policy::OpenStatus status)
{
    using dokany_policy::OpenStatus;
    switch (status) {
    case OpenStatus::success:
        return STATUS_SUCCESS;
    case OpenStatus::name_not_found:
        return STATUS_OBJECT_NAME_NOT_FOUND;
    case OpenStatus::name_collision:
        return STATUS_OBJECT_NAME_COLLISION;
    case OpenStatus::write_protected:
        return STATUS_MEDIA_WRITE_PROTECTED;
    case OpenStatus::file_is_directory:
        return STATUS_FILE_IS_A_DIRECTORY;
    case OpenStatus::not_a_directory:
        return STATUS_NOT_A_DIRECTORY;
    case OpenStatus::invalid_disposition:
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_INVALID_PARAMETER;
}

void debug_open(const MountRuntime& runtime, LPCWSTR file_name, ACCESS_MASK access,
                ULONG disposition, ULONG options, bool exists, NTSTATUS status)
{
    if (!runtime.debug) {
        return;
    }
    std::wcerr << L"[ZwCreateFile] path=" << (file_name ? file_name : L"<null>")
               << L" disposition=" << disposition
               << L" access=0x" << std::hex << access
               << L" options=0x" << options
               << L" exists=" << std::dec << (exists ? 1 : 0)
               << L" -> NTSTATUS=0x" << std::hex << static_cast<unsigned long>(status)
               << std::dec << L'\n';
}

NTSTATUS DOKAN_CALLBACK create_file(LPCWSTR file_name, PDOKAN_IO_SECURITY_CONTEXT,
                                    ACCESS_MASK desired_access, ULONG, ULONG,
                                    ULONG create_disposition, ULONG create_options,
                                    PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!runtime || !path) {
        return STATUS_INVALID_PARAMETER;
    }

    if (const auto target = writable_target(*runtime, *path)) {
        using dokany_policy::CreateDisposition;
        const auto disposition = static_cast<CreateDisposition>(create_disposition);
        const bool directory_only = (create_options & FILE_DIRECTORY_FILE) != 0;
        const bool non_directory_only = (create_options & FILE_NON_DIRECTORY_FILE) != 0;
        const bool creates = disposition == CreateDisposition::create || disposition == CreateDisposition::open_if ||
                             disposition == CreateDisposition::overwrite_if ||
                             disposition == CreateDisposition::supersede;
        if (create_disposition > static_cast<ULONG>(CreateDisposition::overwrite_if)) return STATUS_INVALID_PARAMETER;
        NTSTATUS status = STATUS_SUCCESS;
        auto state = std::make_unique<OpenState>();
        state->partition = target->partition;
        state->pfs_path = target->pfs_path;

        if (target->exists) {
            if (disposition == CreateDisposition::create) return STATUS_OBJECT_NAME_COLLISION;
            if (target->is_directory && non_directory_only) return STATUS_FILE_IS_A_DIRECTORY;
            if (!target->is_directory && directory_only) return STATUS_NOT_A_DIRECTORY;
            state->directory = target->is_directory;
            const bool truncates = disposition == CreateDisposition::overwrite ||
                                   disposition == CreateDisposition::overwrite_if ||
                                   disposition == CreateDisposition::supersede;
            if (!state->directory && truncates) {
                if (const auto opened = ensure_handle(*runtime, *state, true); opened != STATUS_SUCCESS) return opened;
            }
            // Dokany convention: "opened existing" for *_IF dispositions.
            if (disposition == CreateDisposition::open_if || disposition == CreateDisposition::overwrite_if ||
                disposition == CreateDisposition::supersede) {
                status = STATUS_OBJECT_NAME_COLLISION;
            }
        } else {
            if (!creates) return STATUS_OBJECT_NAME_NOT_FOUND;
            if (directory_only) {
                const auto made = runtime->engine->make_directory(target->partition, target->pfs_path);
                if (made != WriteStatus::ok) return map_write_status(made);
                state->directory = true;
            } else {
                const auto created = runtime->engine->open_write(target->partition, target->pfs_path, true, true,
                                                                 state->handle);
                if (created != WriteStatus::ok) return map_write_status(created);
                state->has_handle = true;
            }
        }
        (void)desired_access;
        debug_open(*runtime, file_name, desired_access, create_disposition, create_options, target->exists, status);
        info->IsDirectory = state->directory ? TRUE : FALSE;
        info->Context = reinterpret_cast<ULONG64>(state.release());
        return status;
    }

    const auto node = runtime->lookup(*path);
    dokany_policy::OpenRequest request{};
    request.exists = node.ok;
    request.is_directory = node.ok && node.is_directory();
    request.wants_write = has_write_access(desired_access);
    request.directory_only = (create_options & FILE_DIRECTORY_FILE) != 0;
    request.non_directory_only = (create_options & FILE_NON_DIRECTORY_FILE) != 0;
    request.delete_on_close = (create_options & FILE_DELETE_ON_CLOSE) != 0;
    request.disposition = create_disposition;

    const NTSTATUS status = map_open_status(dokany_policy::evaluate(request));
    debug_open(*runtime, file_name, desired_access, create_disposition, create_options,
               node.ok, status);
    if (status != STATUS_SUCCESS) {
        return status;
    }

    info->IsDirectory = node.is_directory() ? TRUE : FALSE;
    return STATUS_SUCCESS;
}

void DOKAN_CALLBACK cleanup(LPCWSTR, PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    auto* state = open_state(info);
    if (!runtime || !state || !runtime->engine) return;
    if (info->DeletePending) {
        if (state->has_handle) {
            (void)runtime->engine->close(state->handle, true);
            state->has_handle = false;
        }
        (void)(state->directory ? runtime->engine->remove_directory(state->partition, state->pfs_path)
                                : runtime->engine->remove_file(state->partition, state->pfs_path));
        return;
    }
    if (state->has_handle) {
        // The application is done with the file: queue one coalesced commit.
        (void)runtime->engine->close(state->handle);
        state->has_handle = false;
    }
}

void DOKAN_CALLBACK close_file(LPCWSTR, PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    auto* state = open_state(info);
    if (state && state->has_handle && runtime && runtime->engine) {
        (void)runtime->engine->close(state->handle);
    }
    delete state;
    if (info) info->Context = 0;
}

NTSTATUS DOKAN_CALLBACK read_file(LPCWSTR file_name, LPVOID buffer, DWORD buffer_length,
                                  LPDWORD read_length, LONGLONG offset,
                                  PDOKAN_FILE_INFO info)
{
    if (!read_length || offset < 0 || (buffer_length != 0 && !buffer)) {
        return STATUS_INVALID_PARAMETER;
    }
    *read_length = 0;
    auto* runtime = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!runtime || !path) {
        return STATUS_INVALID_PARAMETER;
    }

    auto bytes = std::span<std::byte>(static_cast<std::byte*>(buffer), buffer_length);
    if (auto* state = open_state(info); state && state->has_handle && runtime->engine) {
        std::size_t got = 0;
        const auto status = runtime->engine->read(state->handle, static_cast<std::uint64_t>(offset), bytes, got);
        *read_length = static_cast<DWORD>(got);
        return map_write_status(status);
    }
    const auto result = runtime->read(*path, static_cast<std::uint64_t>(offset), bytes);
    if (!result.ok) {
        const auto node = runtime->lookup(*path);
        return node.ok && node.is_directory() ? STATUS_FILE_IS_A_DIRECTORY
                                              : STATUS_OBJECT_NAME_NOT_FOUND;
    }
    *read_length = static_cast<DWORD>(result.bytes_read);
    if (runtime->debug) {
        std::wcerr << L"[ReadFile] path=" << (file_name ? file_name : L"<null>")
                   << L" offset=" << offset << L" requested=" << buffer_length
                   << L" read=" << *read_length << L'\n';
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK write_file(LPCWSTR, LPCVOID buffer, DWORD length, LPDWORD written,
                                   LONGLONG offset, PDOKAN_FILE_INFO info)
{
    if (written) {
        *written = 0;
    }
    auto* runtime = context(info);
    auto* state = open_state(info);
    if (!runtime || !runtime->engine || !state || state->directory) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    if (!written || (length != 0 && !buffer)) return STATUS_INVALID_PARAMETER;
    if (const auto opened = ensure_handle(*runtime, *state, false); opened != STATUS_SUCCESS) return opened;

    const auto size = runtime->engine->handle_size(state->handle).value_or(0);
    std::uint64_t position = info->WriteToEndOfFile ? size : static_cast<std::uint64_t>(offset);
    std::size_t count = length;
    if (info->PagingIo) {
        // Paging I/O never extends a file (cache manager rounding).
        if (position >= size) return STATUS_SUCCESS;
        count = static_cast<std::size_t>(std::min<std::uint64_t>(count, size - position));
    }
    const auto status = runtime->engine->write(
        state->handle, position, std::span(static_cast<const std::byte*>(buffer), count));
    if (status == WriteStatus::ok) *written = static_cast<DWORD>(count);
    return map_write_status(status);
}

NTSTATUS DOKAN_CALLBACK flush_file_buffers(LPCWSTR, PDOKAN_FILE_INFO)
{
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK get_file_information(LPCWSTR file_name,
                                             LPBY_HANDLE_FILE_INFORMATION buffer,
                                             PDOKAN_FILE_INFO info)
{
    if (!buffer) {
        return STATUS_INVALID_PARAMETER;
    }
    auto* runtime = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!runtime || !path) {
        return STATUS_INVALID_PARAMETER;
    }
    auto node = runtime->lookup(*path);
    if (!node.ok) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (auto* state = open_state(info); state && state->has_handle && runtime->engine) {
        node.size = runtime->engine->handle_size(state->handle).value_or(node.size);
    }
    const bool writable = writable_target(*runtime, *path).has_value();

    std::memset(buffer, 0, sizeof(*buffer));
    buffer->dwFileAttributes = file_attributes(node.is_directory(), writable);
    buffer->nNumberOfLinks = 1;
    buffer->nFileSizeHigh = static_cast<DWORD>(node.size >> 32U);
    buffer->nFileSizeLow = static_cast<DWORD>(node.size & 0xFFFFFFFFULL);
    if (runtime->debug) {
        std::wcerr << L"[GetFileInformation] path=" << (file_name ? file_name : L"<null>")
                   << L" directory=" << (node.is_directory() ? 1 : 0)
                   << L" size=" << node.size << L'\n';
    }
    return STATUS_SUCCESS;
}

void fill_find_data(const MountEntry& entry, WIN32_FIND_DATAW& data, bool writable)
{
    std::memset(&data, 0, sizeof(data));
    data.dwFileAttributes = file_attributes(entry.is_directory(), writable);
    data.nFileSizeHigh = static_cast<DWORD>(entry.size >> 32U);
    data.nFileSizeLow = static_cast<DWORD>(entry.size & 0xFFFFFFFFULL);
    const auto wide_name = utf8_to_wide(entry.name);
    wcsncpy_s(data.cFileName, _countof(data.cFileName), wide_name.c_str(), _TRUNCATE);
}

NTSTATUS DOKAN_CALLBACK find_files(LPCWSTR file_name, PFillFindData fill_find_data_callback,
                                   PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!runtime || !path || !fill_find_data_callback) {
        return STATUS_INVALID_PARAMETER;
    }
    const auto result = runtime->list(*path);
    const bool writable = writable_target(*runtime, *path).has_value();
    if (!result.ok) {
        const auto node = runtime->lookup(*path);
        return node.ok ? STATUS_NOT_A_DIRECTORY : STATUS_OBJECT_PATH_NOT_FOUND;
    }

    if (runtime->debug) {
        std::wcerr << L"[FindFiles] path=" << (file_name ? file_name : L"<null>")
                   << L" entries=" << result.entries.size() << L'\n';
    }
    for (const auto& entry : result.entries) {
        WIN32_FIND_DATAW data{};
        fill_find_data(entry, data, writable);
        if (fill_find_data_callback(&data, info) != 0) {
            break;
        }
    }
    return STATUS_SUCCESS;
}

// Attribute/time/security changes are accepted as no-ops on writable PFS
// paths (PFS keeps its own timestamps); Explorer copies fail without this.
NTSTATUS accept_if_writable(LPCWSTR file_name, PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    const auto path = wide_to_utf8(file_name);
    if (runtime && path && writable_target(*runtime, *path)) return STATUS_SUCCESS;
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK set_file_attributes(LPCWSTR file_name, DWORD, PDOKAN_FILE_INFO info)
{
    return accept_if_writable(file_name, info);
}

NTSTATUS DOKAN_CALLBACK set_file_time(LPCWSTR file_name, const FILETIME*, const FILETIME*, const FILETIME*,
                                      PDOKAN_FILE_INFO info)
{
    return accept_if_writable(file_name, info);
}

NTSTATUS DOKAN_CALLBACK delete_file(LPCWSTR file_name, PDOKAN_FILE_INFO info)
{
    // Dokany asks whether deletion is allowed; the delete runs in Cleanup.
    auto* runtime = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!runtime || !path) return STATUS_INVALID_PARAMETER;
    const auto target = writable_target(*runtime, *path);
    if (!target) return STATUS_MEDIA_WRITE_PROTECTED;
    if (!target->exists) return STATUS_OBJECT_NAME_NOT_FOUND;
    return target->is_directory ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK delete_directory(LPCWSTR file_name, PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!runtime || !path) return STATUS_INVALID_PARAMETER;
    const auto target = writable_target(*runtime, *path);
    if (!target) return STATUS_MEDIA_WRITE_PROTECTED;
    if (!target->exists) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (target->pfs_path == "/") return STATUS_ACCESS_DENIED;
    return runtime->engine->pending_children(target->partition, target->pfs_path).empty()
               ? STATUS_SUCCESS
               : STATUS_DIRECTORY_NOT_EMPTY;
}

NTSTATUS DOKAN_CALLBACK move_file(LPCWSTR file_name, LPCWSTR new_file_name, BOOL replace_if_existing,
                                  PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    const auto from_path = wide_to_utf8(file_name);
    const auto to_path = wide_to_utf8(new_file_name);
    if (!runtime || !from_path || !to_path) return STATUS_INVALID_PARAMETER;
    const auto from = writable_target(*runtime, *from_path);
    const auto to = writable_target(*runtime, *to_path);
    if (!from || !to) return STATUS_MEDIA_WRITE_PROTECTED;
    if (from->partition != to->partition) return STATUS_NOT_SAME_DEVICE;
    if (auto* state = open_state(info); state && state->has_handle) {
        // Commit what was written under the old name before moving it.
        (void)runtime->engine->close(state->handle);
        state->has_handle = false;
    }
    const auto status = runtime->engine->rename(from->partition, from->pfs_path, to->pfs_path,
                                                replace_if_existing != FALSE);
    if (status == WriteStatus::ok) {
        if (auto* state = open_state(info)) state->pfs_path = to->pfs_path;
    }
    return map_write_status(status);
}

NTSTATUS DOKAN_CALLBACK set_end_of_file(LPCWSTR, LONGLONG length, PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    auto* state = open_state(info);
    if (!runtime || !runtime->engine || !state || state->directory) return STATUS_MEDIA_WRITE_PROTECTED;
    if (length < 0) return STATUS_INVALID_PARAMETER;
    if (const auto opened = ensure_handle(*runtime, *state, length == 0); opened != STATUS_SUCCESS) return opened;
    return map_write_status(runtime->engine->set_size(state->handle, static_cast<std::uint64_t>(length)));
}

NTSTATUS DOKAN_CALLBACK set_allocation_size(LPCWSTR, LONGLONG length, PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    auto* state = open_state(info);
    if (!runtime || !runtime->engine || !state || state->directory) return STATUS_MEDIA_WRITE_PROTECTED;
    if (length < 0) return STATUS_INVALID_PARAMETER;
    // Preallocation is a hint (PFS allocates at commit); only shrinking matters.
    if (const auto opened = ensure_handle(*runtime, *state, false); opened != STATUS_SUCCESS) return opened;
    const auto size = runtime->engine->handle_size(state->handle).value_or(0);
    if (static_cast<std::uint64_t>(length) < size) {
        return map_write_status(runtime->engine->set_size(state->handle, static_cast<std::uint64_t>(length)));
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK get_disk_free_space(PULONGLONG free_bytes_available,
                                            PULONGLONG total_bytes,
                                            PULONGLONG total_free_bytes,
                                            PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    if (!runtime || !free_bytes_available || !total_bytes || !total_free_bytes) {
        return STATUS_INVALID_PARAMETER;
    }
    *total_bytes = runtime->session.device().size_bytes();
    *free_bytes_available = 0;
    *total_free_bytes = 0;
    if (runtime->engine) {
        // Report the roomiest writable PFS partition, already net of the
        // reserve and of queued-but-uncommitted data.
        std::uint64_t best = 0;
        for (const auto& partition : runtime->session.scan_result().partitions) {
            if (partition.is_sub() || !runtime->engine->partition_writable(partition.id)) continue;
            best = (std::max)(best, runtime->engine->available_bytes(partition.id).value_or(0));
        }
        *free_bytes_available = best;
        *total_free_bytes = best;
        return STATUS_SUCCESS;
    }
    if (runtime->debug) {
        std::wcerr << L"[GetDiskFreeSpace] total=" << *total_bytes
                   << L" free=0 (read-only view)\n";
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK get_volume_information(LPWSTR volume_name, DWORD volume_name_size,
                                               LPDWORD serial_number,
                                               LPDWORD maximum_component_length,
                                               LPDWORD file_system_flags,
                                               LPWSTR file_system_name,
                                               DWORD file_system_name_size,
                                               PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    if (volume_name && volume_name_size > 0) {
        wcsncpy_s(volume_name, volume_name_size, L"PS2 DriveForge", _TRUNCATE);
    }
    if (serial_number) {
        *serial_number = 0x50533244U;
    }
    if (maximum_component_length) {
        *maximum_component_length = 255;
    }
    if (file_system_flags) {
        *file_system_flags = FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK |
                             (runtime && runtime->engine ? 0U : FILE_READ_ONLY_VOLUME);
    }
    if (file_system_name && file_system_name_size > 0) {
        wcsncpy_s(file_system_name, file_system_name_size, L"PS2PFS", _TRUNCATE);
    }
    if (runtime && runtime->debug) {
        std::wcerr << L"[GetVolumeInformation] volume=PS2 DriveForge fs=PS2PFS read-only\n";
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK mounted(LPCWSTR mount_point, PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    if (runtime && runtime->mounted) {
        runtime->mounted->store(true, std::memory_order_release);
    }
    if (runtime && runtime->debug) {
        std::wcerr << L"[Mounted] " << (mount_point ? mount_point : L"<unknown>") << L'\n';
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK unmounted(PDOKAN_FILE_INFO info)
{
    auto* runtime = context(info);
    if (runtime && runtime->mounted) {
        runtime->mounted->store(false, std::memory_order_release);
    }
    if (runtime && runtime->debug) {
        std::wcerr << L"[Unmounted]\n";
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK set_file_security(LPCWSTR file_name, PSECURITY_INFORMATION,
                                          PSECURITY_DESCRIPTOR, ULONG,
                                          PDOKAN_FILE_INFO info)
{
    return accept_if_writable(file_name, info);
}

DOKAN_OPERATIONS make_operations()
{
    DOKAN_OPERATIONS operations{};
    operations.ZwCreateFile = create_file;
    operations.Cleanup = cleanup;
    operations.CloseFile = close_file;
    operations.ReadFile = read_file;
    operations.WriteFile = write_file;
    operations.FlushFileBuffers = flush_file_buffers;
    operations.GetFileInformation = get_file_information;
    operations.FindFiles = find_files;
    operations.SetFileAttributes = set_file_attributes;
    operations.SetFileTime = set_file_time;
    operations.DeleteFile = delete_file;
    operations.DeleteDirectory = delete_directory;
    operations.MoveFile = move_file;
    operations.SetEndOfFile = set_end_of_file;
    operations.SetAllocationSize = set_allocation_size;
    operations.GetDiskFreeSpace = get_disk_free_space;
    operations.GetVolumeInformation = get_volume_information;
    operations.Mounted = mounted;
    operations.Unmounted = unmounted;
    operations.SetFileSecurity = set_file_security;
    return operations;
}

} // namespace

struct DokanyMountController::Impl {
    std::unique_ptr<MountRuntime> runtime;
    std::thread worker;
    std::atomic_bool running{false};
    std::atomic_bool mounted{false};
    std::atomic_int status{DOKAN_SUCCESS};
    mutable std::mutex state_mutex;
    std::string error;

    void set_error(std::string value)
    {
        std::scoped_lock lock(state_mutex);
        error = std::move(value);
    }

    void run()
    {
        DokanInit();

        DOKAN_OPTIONS options{};
        options.Version = DOKAN_VERSION;
        options.Options = runtime->engine ? DOKAN_OPTION_MOUNT_MANAGER
                                          : DOKAN_OPTION_WRITE_PROTECT | DOKAN_OPTION_MOUNT_MANAGER;
        options.GlobalContext = reinterpret_cast<ULONG64>(runtime.get());
        options.MountPoint = runtime->mount_point.c_str();
        // Backstop only: no callback waits on the PS2 disk in write-back mode,
        // but a cold first listing on an old bridge may take a while.
        options.Timeout = runtime->engine ? 60000 : 15000;
        options.AllocationUnitSize = 8192;
        options.SectorSize = apa::kSectorSize;

        auto operations = make_operations();
        const int result = DokanMain(&options, &operations);
        status.store(result, std::memory_order_release);
        mounted.store(false, std::memory_order_release);
        running.store(false, std::memory_order_release);
        if (result != DOKAN_SUCCESS) {
            set_error("Dokany mount failed with code " + std::to_string(result));
        }
        DokanShutdown();
    }
};

DokanyMountController::DokanyMountController() : impl_(std::make_unique<Impl>()) {}

DokanyMountController::~DokanyMountController()
{
    if (impl_->running.load(std::memory_order_acquire)) {
        request_unmount();
    }
    wait();
}

bool DokanyMountController::start_writable(std::unique_ptr<BlockDevice> source,
                                           WriterFactory factory, WriteBackOptions options,
                                           std::wstring mount_point, bool debug)
{
    pending_writable_ = std::make_unique<WritableSetup>(WritableSetup{std::move(factory), std::move(options)});
    return start(std::move(source), std::move(mount_point), debug);
}

PfsWriteBack* DokanyMountController::write_back() noexcept
{
    return impl_->runtime ? impl_->runtime->engine.get() : nullptr;
}

bool DokanyMountController::start(std::unique_ptr<BlockDevice> source,
                                  std::wstring mount_point, bool debug)
{
    auto writable_setup = std::move(pending_writable_);
    if (!source) {
        impl_->set_error("No source device was supplied");
        return false;
    }
    if (impl_->worker.joinable() || impl_->running.load(std::memory_order_acquire)) {
        impl_->set_error("A Dokany mount is already active");
        return false;
    }

    mount_point = normalize_mount_point(std::move(mount_point));
    if (mount_point.empty()) {
        impl_->set_error("No free mount point is available");
        return false;
    }

    impl_->runtime = std::make_unique<MountRuntime>(std::move(source), std::move(mount_point),
                                                    debug, &impl_->mounted);
    if (!impl_->runtime->session.scan()) {
        impl_->set_error("PS2 APA scan failed: " + impl_->runtime->session.last_error());
        impl_->runtime.reset();
        return false;
    }
    if (!impl_->runtime->session.scan_result().mbr_valid) {
        impl_->set_error("The selected source does not contain a valid PS2 APA MBR");
        impl_->runtime.reset();
        return false;
    }
    if (writable_setup) {
        if (!impl_->runtime->session.scan_result().ok()) {
            impl_->set_error("Refusing a writable mount: APA diagnostics are not clean");
            impl_->runtime.reset();
            return false;
        }
        impl_->runtime->engine = std::make_unique<PfsWriteBack>(
            impl_->runtime->session, std::move(writable_setup->factory), std::move(writable_setup->options));
        impl_->runtime->writable =
            std::make_unique<WritableMountView>(impl_->runtime->session, *impl_->runtime->engine);
    }

    {
        std::scoped_lock lock(impl_->state_mutex);
        impl_->error.clear();
    }
    impl_->status.store(DOKAN_SUCCESS, std::memory_order_release);
    impl_->mounted.store(false, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
    return true;
}

bool DokanyMountController::request_unmount()
{
    if (!impl_->running.load(std::memory_order_acquire) || !impl_->runtime) {
        return true;
    }
    return DokanRemoveMountPoint(impl_->runtime->mount_point.c_str()) != FALSE;
}

void DokanyMountController::wait()
{
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    if (!impl_->running.load(std::memory_order_acquire)) {
        impl_->runtime.reset();
    }
}

bool DokanyMountController::is_running() const noexcept
{
    return impl_->running.load(std::memory_order_acquire);
}

bool DokanyMountController::is_mounted() const noexcept
{
    return impl_->mounted.load(std::memory_order_acquire);
}

int DokanyMountController::last_status() const noexcept
{
    return impl_->status.load(std::memory_order_acquire);
}

std::string DokanyMountController::last_error() const
{
    std::scoped_lock lock(impl_->state_mutex);
    return impl_->error;
}

std::wstring DokanyMountController::mount_point() const
{
    return impl_->runtime ? impl_->runtime->mount_point : std::wstring{};
}

std::wstring DokanyMountController::normalize_mount_point(std::wstring value)
{
    if (value.size() == 2 && value[1] == L':') {
        value.push_back(L'\\');
    }
    if (value.size() == 3 && value[1] == L':' && value[2] == L'\\') {
        value[0] = static_cast<wchar_t>(towupper(value[0]));
        return value;
    }
    return {};
}

std::wstring DokanyMountController::suggest_mount_point(wchar_t)
{
    const auto letter = darkness_policy::choose_mount_letter(GetLogicalDrives());
    if (!letter) {
        return {};
    }
    return std::wstring{*letter, L':', L'\\'};
}

bool remove_dokany_mount_point(std::wstring mount_point)
{
    mount_point = DokanyMountController::normalize_mount_point(std::move(mount_point));
    if (mount_point.empty()) {
        return false;
    }
    DokanInit();
    const bool ok = DokanRemoveMountPoint(mount_point.c_str()) != FALSE;
    DokanShutdown();
    return ok;
}

} // namespace ps2hdd

#endif