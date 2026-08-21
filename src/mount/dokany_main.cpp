#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/mount_view.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/version.hpp"

#if __has_include(<dokan/dokan.h>)
#include <dokan/dokan.h>
#else
#include <dokan.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

struct MountContext {
    explicit MountContext(std::unique_ptr<ps2hdd::BlockDevice> source)
        : session(std::move(source)), view(session)
    {
    }

    ps2hdd::DriveSession session;
    ps2hdd::ReadOnlyMountView view;
};

std::wstring g_mount_point;

MountContext* context(PDOKAN_FILE_INFO info)
{
    if (!info || !info->DokanOptions) {
        return nullptr;
    }
    return reinterpret_cast<MountContext*>(info->DokanOptions->GlobalContext);
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
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, length, result.data(), bytes,
                            nullptr, nullptr) != bytes) {
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

    // PFS stores byte strings and legacy software may contain names that are not
    // valid UTF-8. Keep the mount operational by exposing those bytes in the low
    // Unicode range. Darkness keeps this fallback explicit; a later naming pass
    // can add a documented legacy encoding policy without changing PFS parsing.
    std::wstring fallback;
    fallback.reserve(value.size());
    for (const unsigned char byte : value) {
        fallback.push_back(static_cast<wchar_t>(byte));
    }
    return fallback;
}

DWORD file_attributes(bool directory)
{
    // FILE_ATTRIBUTE_NORMAL must be used alone. The mount is read-only, so files
    // carry READONLY and directories carry READONLY|DIRECTORY instead.
    return FILE_ATTRIBUTE_READONLY | (directory ? FILE_ATTRIBUTE_DIRECTORY : 0U);
}

bool has_write_access(ACCESS_MASK access)
{
    constexpr ACCESS_MASK write_mask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
                                       FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER |
                                       GENERIC_WRITE;
    return (access & write_mask) != 0;
}

NTSTATUS DOKAN_CALLBACK create_file(LPCWSTR file_name,
                                    PDOKAN_IO_SECURITY_CONTEXT,
                                    ACCESS_MASK desired_access,
                                    ULONG,
                                    ULONG,
                                    ULONG create_disposition,
                                    ULONG create_options,
                                    PDOKAN_FILE_INFO info)
{
    auto* ctx = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!ctx || !path) {
        return STATUS_INVALID_PARAMETER;
    }

    const auto node = ctx->view.lookup(*path);
    if (!node.ok) {
        return create_disposition == OPEN_EXISTING ? STATUS_OBJECT_NAME_NOT_FOUND
                                                    : STATUS_MEDIA_WRITE_PROTECTED;
    }

    if (create_disposition == CREATE_NEW) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    if (create_disposition == CREATE_ALWAYS || create_disposition == TRUNCATE_EXISTING ||
        has_write_access(desired_access)) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }

    if ((create_options & FILE_NON_DIRECTORY_FILE) != 0 && node.is_directory()) {
        return STATUS_FILE_IS_A_DIRECTORY;
    }
    if ((create_options & FILE_DIRECTORY_FILE) != 0 && !node.is_directory()) {
        return STATUS_NOT_A_DIRECTORY;
    }
    info->IsDirectory = node.is_directory() ? TRUE : FALSE;
    return STATUS_SUCCESS;
}

void DOKAN_CALLBACK cleanup(LPCWSTR, PDOKAN_FILE_INFO) {}
void DOKAN_CALLBACK close_file(LPCWSTR, PDOKAN_FILE_INFO) {}

NTSTATUS DOKAN_CALLBACK read_file(LPCWSTR file_name,
                                  LPVOID buffer,
                                  DWORD buffer_length,
                                  LPDWORD read_length,
                                  LONGLONG offset,
                                  PDOKAN_FILE_INFO info)
{
    if (!read_length || offset < 0 || (buffer_length != 0 && !buffer)) {
        return STATUS_INVALID_PARAMETER;
    }
    *read_length = 0;
    auto* ctx = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!ctx || !path) {
        return STATUS_INVALID_PARAMETER;
    }

    auto bytes = std::span<std::byte>(static_cast<std::byte*>(buffer), buffer_length);
    const auto result = ctx->view.read_file(*path, static_cast<std::uint64_t>(offset), bytes);
    if (!result.ok) {
        const auto node = ctx->view.lookup(*path);
        if (node.ok && node.is_directory()) {
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    *read_length = static_cast<DWORD>(result.bytes_read);
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK write_file(LPCWSTR, LPCVOID, DWORD, LPDWORD written,
                                   LONGLONG, PDOKAN_FILE_INFO)
{
    if (written) {
        *written = 0;
    }
    return STATUS_MEDIA_WRITE_PROTECTED;
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
    auto* ctx = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!ctx || !path) {
        return STATUS_INVALID_PARAMETER;
    }
    const auto node = ctx->view.lookup(*path);
    if (!node.ok) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    std::memset(buffer, 0, sizeof(*buffer));
    buffer->dwFileAttributes = file_attributes(node.is_directory());
    buffer->nNumberOfLinks = 1;
    buffer->nFileSizeHigh = static_cast<DWORD>(node.size >> 32U);
    buffer->nFileSizeLow = static_cast<DWORD>(node.size & 0xFFFFFFFFULL);
    return STATUS_SUCCESS;
}

void fill_find_data(const ps2hdd::MountEntry& entry, WIN32_FIND_DATAW& data)
{
    std::memset(&data, 0, sizeof(data));
    data.dwFileAttributes = file_attributes(entry.is_directory());
    data.nFileSizeHigh = static_cast<DWORD>(entry.size >> 32U);
    data.nFileSizeLow = static_cast<DWORD>(entry.size & 0xFFFFFFFFULL);
    const auto wide_name = utf8_to_wide(entry.name);
    wcsncpy_s(data.cFileName, _countof(data.cFileName), wide_name.c_str(), _TRUNCATE);
}

NTSTATUS DOKAN_CALLBACK find_files(LPCWSTR file_name,
                                   PFillFindData fill_find_data_callback,
                                   PDOKAN_FILE_INFO info)
{
    auto* ctx = context(info);
    const auto path = wide_to_utf8(file_name);
    if (!ctx || !path || !fill_find_data_callback) {
        return STATUS_INVALID_PARAMETER;
    }
    const auto result = ctx->view.list_directory(*path);
    if (!result.ok) {
        const auto node = ctx->view.lookup(*path);
        return node.ok ? STATUS_NOT_A_DIRECTORY : STATUS_OBJECT_PATH_NOT_FOUND;
    }

    for (const auto& entry : result.entries) {
        WIN32_FIND_DATAW data{};
        fill_find_data(entry, data);
        if (fill_find_data_callback(&data, info) != 0) {
            break;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK set_file_attributes(LPCWSTR, DWORD, PDOKAN_FILE_INFO)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK set_file_time(LPCWSTR, const FILETIME*, const FILETIME*, const FILETIME*,
                                      PDOKAN_FILE_INFO)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK delete_file(LPCWSTR, PDOKAN_FILE_INFO)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK delete_directory(LPCWSTR, PDOKAN_FILE_INFO)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK move_file(LPCWSTR, LPCWSTR, BOOL, PDOKAN_FILE_INFO)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK set_end_of_file(LPCWSTR, LONGLONG, PDOKAN_FILE_INFO)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK set_allocation_size(LPCWSTR, LONGLONG, PDOKAN_FILE_INFO)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS DOKAN_CALLBACK get_disk_free_space(PULONGLONG free_bytes_available,
                                            PULONGLONG total_bytes,
                                            PULONGLONG total_free_bytes,
                                            PDOKAN_FILE_INFO info)
{
    auto* ctx = context(info);
    if (!ctx || !free_bytes_available || !total_bytes || !total_free_bytes) {
        return STATUS_INVALID_PARAMETER;
    }
    *total_bytes = ctx->session.device().size_bytes();
    *free_bytes_available = 0;
    *total_free_bytes = 0;
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK get_volume_information(LPWSTR volume_name,
                                               DWORD volume_name_size,
                                               LPDWORD serial_number,
                                               LPDWORD maximum_component_length,
                                               LPDWORD file_system_flags,
                                               LPWSTR file_system_name,
                                               DWORD file_system_name_size,
                                               PDOKAN_FILE_INFO)
{
    if (volume_name && volume_name_size > 0) {
        wcsncpy_s(volume_name, volume_name_size, L"PS2 DriveForge", _TRUNCATE);
    }
    if (serial_number) {
        *serial_number = 0x50533244U; // "PS2D"
    }
    if (maximum_component_length) {
        *maximum_component_length = 255;
    }
    if (file_system_flags) {
        *file_system_flags = FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK |
                             FILE_READ_ONLY_VOLUME;
    }
    if (file_system_name && file_system_name_size > 0) {
        wcsncpy_s(file_system_name, file_system_name_size, L"PS2PFS", _TRUNCATE);
    }
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK mounted(LPCWSTR mount_point, PDOKAN_FILE_INFO)
{
    std::wcout << L"Mounted PS2 DriveForge read-only at "
               << (mount_point ? mount_point : L"<unknown>") << L"\n";
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK unmounted(PDOKAN_FILE_INFO)
{
    std::wcout << L"PS2 DriveForge filesystem unmounted.\n";
    return STATUS_SUCCESS;
}

NTSTATUS DOKAN_CALLBACK set_file_security(LPCWSTR, PSECURITY_INFORMATION,
                                          PSECURITY_DESCRIPTOR, ULONG, PDOKAN_FILE_INFO)
{
    return STATUS_MEDIA_WRITE_PROTECTED;
}

BOOL WINAPI console_handler(DWORD control_type)
{
    if (control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
        control_type == CTRL_CLOSE_EVENT || control_type == CTRL_SHUTDOWN_EVENT) {
        if (!g_mount_point.empty()) {
            DokanRemoveMountPoint(g_mount_point.c_str());
        }
        return TRUE;
    }
    return FALSE;
}

std::wstring normalize_mount_point(std::wstring value)
{
    if (value.size() == 2 && value[1] == L':') {
        value.push_back(L'\\');
    }
    return value;
}

void usage()
{
    std::wcout << L"PS2 DriveForge " << utf8_to_wide(ps2hdd::version::string)
               << L"-dev \"" << utf8_to_wide(ps2hdd::version::codename)
               << L"\" - read-only Dokany mount\n\n"
                  L"Usage:\n"
                  L"  PS2-DriveForge-Mount.exe --image <disk.img> --mount <P:>\n"
                  L"  PS2-DriveForge-Mount.exe --physical <index> --mount <P:>\n"
                  L"  PS2-DriveForge-Mount.exe --unmount <P:>\n\n"
                  L"The source device is always opened read-only. All mounted mutation requests are rejected.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::optional<std::filesystem::path> image;
    std::optional<unsigned> physical;
    std::optional<std::wstring> mount_point;
    std::optional<std::wstring> unmount_point;

    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg(argv[i]);
        if (arg == L"--image" && i + 1 < argc) {
            image = std::filesystem::path(argv[++i]);
        } else if (arg == L"--physical" && i + 1 < argc) {
            try {
                physical = static_cast<unsigned>(std::stoul(argv[++i]));
            } catch (...) {
                std::wcerr << L"Invalid PhysicalDrive index.\n";
                return 2;
            }
        } else if (arg == L"--mount" && i + 1 < argc) {
            mount_point = normalize_mount_point(argv[++i]);
        } else if (arg == L"--unmount" && i + 1 < argc) {
            unmount_point = normalize_mount_point(argv[++i]);
        } else if (arg == L"--help" || arg == L"-h") {
            usage();
            return 0;
        } else {
            std::wcerr << L"Unknown or incomplete option: " << arg << L"\n";
            usage();
            return 2;
        }
    }

    DokanInit();
    if (unmount_point) {
        const bool ok = DokanRemoveMountPoint(unmount_point->c_str()) != FALSE;
        DokanShutdown();
        if (!ok) {
            std::wcerr << L"Could not unmount " << *unmount_point << L"\n";
            return 1;
        }
        std::wcout << L"Unmount requested for " << *unmount_point << L"\n";
        return 0;
    }

    if (!mount_point || (image.has_value() == physical.has_value())) {
        DokanShutdown();
        usage();
        return 2;
    }

    std::unique_ptr<ps2hdd::BlockDevice> source;
    if (image) {
        auto device = std::make_unique<ps2hdd::FileBlockDevice>(*image);
        if (!device->is_open()) {
            DokanShutdown();
            std::wcerr << L"Could not open disk image.\n";
            return 1;
        }
        source = std::move(device);
    } else {
        auto device = std::make_unique<ps2hdd::PhysicalDrive>(*physical);
        if (!device->is_open()) {
            DokanShutdown();
            std::wcerr << L"Could not open PhysicalDrive" << *physical << L" read-only.\n";
            return 1;
        }
        source = std::move(device);
    }

    MountContext ctx(std::move(source));
    if (!ctx.session.scan()) {
        DokanShutdown();
        std::cerr << "PS2 APA scan failed: " << ctx.session.last_error() << '\n';
        return 1;
    }

    DOKAN_OPTIONS options{};
    options.Version = DOKAN_VERSION;
    options.Options = DOKAN_OPTION_WRITE_PROTECT | DOKAN_OPTION_MOUNT_MANAGER;
    options.GlobalContext = reinterpret_cast<ULONG64>(&ctx);
    options.MountPoint = mount_point->c_str();
    options.Timeout = 15000;
    options.AllocationUnitSize = 8192;
    options.SectorSize = ps2hdd::apa::kSectorSize;

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

    g_mount_point = *mount_point;
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::wcout << L"Mounting " << utf8_to_wide(ctx.session.device().display_name())
               << L" at " << *mount_point << L" (READ ONLY)...\n"
               << L"Namespace root: " << *mount_point << L"Partitions\\\n"
               << L"Press Ctrl+C or run --unmount " << *mount_point
               << L" from another terminal.\n";

    const int status = DokanMain(&options, &operations);
    SetConsoleCtrlHandler(console_handler, FALSE);
    g_mount_point.clear();
    DokanShutdown();

    if (status != DOKAN_SUCCESS) {
        std::wcerr << L"Dokany mount failed with code " << status << L".\n";
        return 1;
    }
    return 0;
}
