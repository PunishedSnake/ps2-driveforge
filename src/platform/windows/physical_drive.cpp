#ifdef _WIN32

#include "ps2hdd/physical_drive.hpp"

#include <winioctl.h>

#include <algorithm>
#include <limits>

namespace ps2hdd {
namespace {

class ScopedEvent final {
public:
    ScopedEvent() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ScopedEvent()
    {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_{};
};

void set_overlapped_offset(OVERLAPPED& overlapped, std::uint64_t offset) noexcept
{
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
}

bool finish_overlapped(HANDLE handle, OVERLAPPED& overlapped, BOOL started,
                       DWORD& transferred) noexcept
{
    if (!started) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            return false;
        }
    }
    return GetOverlappedResult(handle, &overlapped, &transferred, TRUE) != FALSE;
}

} // namespace

PhysicalDrive::PhysicalDrive(unsigned index) : index_(index)
{
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index_);

    // Read-only is a structural invariant. Emilia only changes how reads are
    // scheduled: the raw disk handle still requests GENERIC_READ and no source
    // write capability is introduced.
    handle_ = CreateFileW(path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        open_error_ = GetLastError();
        return;
    }

    ScopedEvent event;
    if (!event.valid()) {
        open_error_ = GetLastError();
        return;
    }

    GET_LENGTH_INFORMATION length{};
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD returned = 0;
    const BOOL started = DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO,
                                         nullptr, 0, &length, sizeof(length),
                                         nullptr, &overlapped);
    if (finish_overlapped(handle_, overlapped, started, returned)) {
        size_ = static_cast<std::uint64_t>(length.Length.QuadPart);
        open_error_ = ERROR_SUCCESS;
    } else {
        open_error_ = GetLastError();
    }
}

PhysicalDrive::~PhysicalDrive()
{
    if (handle_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(handle_, nullptr);
        CloseHandle(handle_);
    }
}

bool PhysicalDrive::is_open() const noexcept
{
    return handle_ != INVALID_HANDLE_VALUE && size_ != 0;
}

DWORD PhysicalDrive::open_error() const noexcept
{
    return open_error_;
}

unsigned PhysicalDrive::index() const noexcept
{
    return index_;
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
    if (out.empty()) {
        return true;
    }

    ScopedEvent event;
    if (!event.valid()) {
        return false;
    }

    std::size_t done = 0;
    while (done < out.size()) {
        const auto remaining = out.size() - done;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));

        if (!ResetEvent(event.get())) {
            return false;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        set_overlapped_offset(overlapped, offset + done);

        const BOOL started = ReadFile(handle_, out.data() + done, chunk, nullptr, &overlapped);
        DWORD read_bytes = 0;
        if (!finish_overlapped(handle_, overlapped, started, read_bytes) || read_bytes == 0) {
            return false;
        }
        done += read_bytes;
    }
    return true;
}

} // namespace ps2hdd

#endif
