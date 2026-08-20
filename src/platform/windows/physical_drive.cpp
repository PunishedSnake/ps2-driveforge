#ifdef _WIN32

#include "ps2hdd/physical_drive.hpp"

#include <winioctl.h>

#include <algorithm>
#include <limits>
#include <sstream>

namespace ps2hdd {

PhysicalDrive::PhysicalDrive(unsigned index) : index_(index)
{
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index_);

    // Read-only is a structural invariant during Ayanami/Bocchi/Chisato, not a
    // UI preference. Do not change GENERIC_READ to GENERIC_READ|GENERIC_WRITE
    // when adding future mutation support; writable devices need a separate,
    // explicitly gated capability with metadata backup and validation.
    handle_ = CreateFileW(path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    GET_LENGTH_INFORMATION length{};
    DWORD returned = 0;
    if (DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO,
                        nullptr, 0, &length, sizeof(length), &returned, nullptr)) {
        size_ = static_cast<std::uint64_t>(length.Length.QuadPart);
    }
}

PhysicalDrive::~PhysicalDrive()
{
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
    }
}

bool PhysicalDrive::is_open() const noexcept
{
    return handle_ != INVALID_HANDLE_VALUE && size_ != 0;
}

std::uint64_t PhysicalDrive::size_bytes() const
{
    return size_;
}

std::string PhysicalDrive::display_name() const
{
    return "\\\\.\\PhysicalDrive" + std::to_string(index_);
}

bool PhysicalDrive::read(std::uint64_t offset, std::span<std::byte> out)
{
    if (!is_open() || offset > size_ || out.size() > size_ - offset) {
        return false;
    }

    // SetFilePointerEx changes shared HANDLE state, therefore seek+ReadFile must
    // currently be serialized as one operation. This is correct but intentionally
    // conservative: it is also one of the known performance bottlenecks tracked
    // in docs/performance.md. A future offset/overlapped backend should remove
    // this serialization without changing BlockDevice's byte-addressed contract.
    std::scoped_lock lock(mutex_);

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) {
        return false;
    }

    std::size_t done = 0;
    while (done < out.size()) {
        const auto remaining = out.size() - done;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
        DWORD read_bytes = 0;
        if (!ReadFile(handle_, out.data() + done, chunk, &read_bytes, nullptr) || read_bytes == 0) {
            return false;
        }
        done += read_bytes;
    }
    return true;
}

} // namespace ps2hdd

#endif
