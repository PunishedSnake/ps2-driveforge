#ifdef _WIN32

#include "ps2hdd/windows_partition_volume.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <array>
#include <string>
#include <vector>

namespace ps2hdd {

std::optional<std::filesystem::path> find_partition_volume(unsigned disk_number, std::uint64_t start_offset_bytes)
{
    std::array<wchar_t, MAX_PATH> name{};
    HANDLE search = FindFirstVolumeW(name.data(), static_cast<DWORD>(name.size()));
    if (search == INVALID_HANDLE_VALUE) return std::nullopt;
    std::optional<std::filesystem::path> found;
    do {
        std::wstring volume(name.data());
        if (volume.empty()) continue;
        std::wstring device = volume;
        if (device.back() == L'\\') device.pop_back(); // CreateFile wants no trailing slash
        HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                    nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        std::vector<std::byte> buffer(sizeof(VOLUME_DISK_EXTENTS) + 8 * sizeof(DISK_EXTENT));
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buffer.data(),
                                        static_cast<DWORD>(buffer.size()), &returned, nullptr);
        CloseHandle(handle);
        if (!ok) continue;
        const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
        if (extents->NumberOfDiskExtents < 1 || extents->Extents[0].DiskNumber != disk_number ||
            static_cast<std::uint64_t>(extents->Extents[0].StartingOffset.QuadPart) != start_offset_bytes) {
            continue;
        }
        std::array<wchar_t, 1024> paths{};
        DWORD length = 0;
        if (GetVolumePathNamesForVolumeNameW(volume.c_str(), paths.data(), static_cast<DWORD>(paths.size()), &length) &&
            paths[0] != L'\0') {
            found = std::filesystem::path(paths.data());
        } else {
            found = std::filesystem::path(volume);
        }
        break;
    } while (FindNextVolumeW(search, name.data(), static_cast<DWORD>(name.size())));
    FindVolumeClose(search);
    return found;
}

} // namespace ps2hdd

#endif
