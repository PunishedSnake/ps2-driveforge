#include "ps2hdd/physical_discovery.hpp"

#ifdef _WIN32

#include "ps2hdd/apa.hpp"
#include "ps2hdd/physical_drive.hpp"

#include <windows.h>
#include <setupapi.h>
#include <ntddstor.h>
#include <winioctl.h>

#include <array>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace ps2hdd {
namespace {

std::string utf8_from_wide(const wchar_t* text)
{
    if (!text || *text == L'\0') {
        return {};
    }
    const int length = static_cast<int>(wcslen(text));
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length,
                            result.data(), bytes, nullptr, nullptr) != bytes) {
        return {};
    }
    return result;
}

std::string device_name(HDEVINFO devices, SP_DEVINFO_DATA& info)
{
    std::array<wchar_t, 512> buffer{};
    DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_FRIENDLYNAME,
                                          &type, reinterpret_cast<PBYTE>(buffer.data()),
                                          static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                                          nullptr)) {
        return utf8_from_wide(buffer.data());
    }
    buffer.fill(L'\0');
    if (SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_DEVICEDESC,
                                          &type, reinterpret_cast<PBYTE>(buffer.data()),
                                          static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                                          nullptr)) {
        return utf8_from_wide(buffer.data());
    }
    return {};
}

} // namespace

std::vector<PhysicalDriveProbe> discover_physical_drives(unsigned max_index)
{
    std::vector<PhysicalDriveProbe> result;
    std::set<unsigned> seen;

    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return result;
    }

    for (DWORD ordinal = 0;; ++ordinal) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &GUID_DEVINTERFACE_DISK,
                                         ordinal, &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, nullptr, 0, &required, nullptr);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        // SetupDiGetDeviceInterfaceDetailW wants a variable-size structure whose
        // first member still has normal pointer alignment. A byte vector is not
        // guaranteed to provide that alignment, so reserve in max_align_t units.
        const std::size_t units =
            (static_cast<std::size_t>(required) + sizeof(std::max_align_t) - 1U) /
            sizeof(std::max_align_t);
        std::vector<std::max_align_t> storage(units);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device_info{};
        device_info.cbSize = sizeof(device_info);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interface_data, detail, required,
                                              nullptr, &device_info)) {
            continue;
        }

        // No data access is requested here. The interface handle exists only to
        // ask Windows which PhysicalDrive number backs this concrete disk device.
        HANDLE interface_handle = CreateFileW(detail->DevicePath, 0,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                                               nullptr, OPEN_EXISTING,
                                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (interface_handle == INVALID_HANDLE_VALUE) {
            continue;
        }

        STORAGE_DEVICE_NUMBER number{};
        DWORD returned = 0;
        const BOOL number_ok = DeviceIoControl(interface_handle, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                               nullptr, 0, &number, sizeof(number),
                                               &returned, nullptr);
        CloseHandle(interface_handle);
        if (!number_ok || number.DeviceType != FILE_DEVICE_DISK || number.DeviceNumber >= max_index) {
            continue;
        }
        const unsigned index = static_cast<unsigned>(number.DeviceNumber);
        if (!seen.insert(index).second) {
            continue;
        }

        PhysicalDriveProbe probe;
        probe.index = index;
        probe.friendly_name = device_name(devices, device_info);

        PhysicalDrive drive(index);
        if (!drive.is_open()) {
            probe.access_denied = drive.open_error() == ERROR_ACCESS_DENIED;
            probe.note = probe.access_denied ? "raw read access denied" : "could not open raw disk read-only";
            result.emplace_back(std::move(probe));
            continue;
        }

        probe.opened = true;
        probe.size_bytes = drive.size_bytes();
        probe.storage = drive.storage_characteristics();

        apa::Reader reader(drive);
        const auto scan = reader.scan();
        probe.apa_detected = scan.mbr_valid;
        probe.apa_clean = scan.ok();
        probe.apa_version = scan.apa_version;
        probe.partition_count = scan.partitions.size();
        for (const auto& partition : scan.partitions) {
            if (!partition.is_sub()) {
                ++probe.main_partition_count;
            }
        }

        if (!scan.mbr_valid) {
            probe.note = "not a PS2 APA disk";
        } else if (!scan.ok()) {
            probe.note = "APA detected with fatal diagnostics";
        } else {
            probe.note = "PS2 APA disk";
        }
        result.emplace_back(std::move(probe));
    }

    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

} // namespace ps2hdd

#endif
