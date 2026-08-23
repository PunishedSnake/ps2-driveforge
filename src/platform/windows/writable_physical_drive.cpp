#ifdef _WIN32

#include "ps2hdd/writable_physical_drive.hpp"

#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

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

bool device_io_overlapped(HANDLE handle, DWORD code,
                          void* output, DWORD output_bytes,
                          DWORD& returned) noexcept
{
    ScopedEvent event;
    if (!event.valid()) {
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    const BOOL started = DeviceIoControl(handle, code,
                                         nullptr, 0,
                                         output, output_bytes,
                                         nullptr, &overlapped);
    return finish_overlapped(handle, overlapped, started, returned);
}

bool volume_uses_disk(HANDLE volume, unsigned disk_index, bool& uses_target)
{
    uses_target = false;
    std::vector<std::byte> buffer(64U * 1024U);
    DWORD returned = 0;
    if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size()),
                         &returned, nullptr)) {
        return false;
    }
    if (returned < sizeof(VOLUME_DISK_EXTENTS)) {
        return false;
    }

    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    const auto required = offsetof(VOLUME_DISK_EXTENTS, Extents) +
                          static_cast<std::size_t>(extents->NumberOfDiskExtents) *
                              sizeof(DISK_EXTENT);
    if (required > returned || required > buffer.size()) {
        return false;
    }

    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
        if (extents->Extents[i].DiskNumber == disk_index) {
            uses_target = true;
            break;
        }
    }
    return true;
}

std::wstring volume_open_path(std::wstring volume_name)
{
    if (!volume_name.empty() && volume_name.back() == L'\\') {
        volume_name.pop_back();
    }
    return volume_name;
}

} // namespace

WritablePhysicalDrive::WritablePhysicalDrive(unsigned index,
                                             const WritablePhysicalDriveOptions& options)
    : index_(index)
{
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index_);
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS |
                              FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        open_error_ = GetLastError();
        admission_error_ = "could not open the physical disk for explicit read/write access";
        return;
    }

    GET_LENGTH_INFORMATION length{};
    DWORD returned = 0;
    if (!device_io_overlapped(handle_, IOCTL_DISK_GET_LENGTH_INFO,
                              &length, sizeof(length), returned)) {
        open_error_ = GetLastError();
        admission_error_ = "could not query physical disk length after RW open";
        close_device();
        return;
    }
    size_ = static_cast<std::uint64_t>(length.Length.QuadPart);

    DISK_GEOMETRY geometry{};
    returned = 0;
    if (!device_io_overlapped(handle_, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                              &geometry, sizeof(geometry), returned)) {
        open_error_ = GetLastError();
        admission_error_ = "could not query physical disk sector geometry";
        close_device();
        return;
    }
    logical_sector_size_ = geometry.BytesPerSector;
    if (logical_sector_size_ != 512U) {
        open_error_ = ERROR_NOT_SUPPORTED;
        admission_error_ = "physical PS2 writes require a 512-byte logical sector device";
        close_device();
        return;
    }

    ready_ = true;
    std::string verification_error;
    if (!physical_write::verify_authorization(*this, options.authorization, verification_error)) {
        open_error_ = ERROR_INVALID_DATA;
        admission_error_ = "RW reopen failed source-identity verification: " + verification_error;
        close_device();
        return;
    }

    if (options.lock_mounted_volumes && !lock_target_volumes(options.dismount_locked_volumes)) {
        if (open_error_ == ERROR_SUCCESS) {
            open_error_ = ERROR_SHARING_VIOLATION;
        }
        close_device();
        return;
    }

    // Locking/dismounting is intentionally between two identity checks. A disk
    // swap in that interval is unlikely but inexpensive to detect, and storage
    // code should not use probability as an access-control primitive.
    if (!physical_write::verify_authorization(*this, options.authorization, verification_error)) {
        open_error_ = ERROR_INVALID_DATA;
        admission_error_ = "physical disk changed while the write lease was being acquired: " +
                           verification_error;
        release_volume_locks();
        close_device();
        return;
    }

    open_error_ = ERROR_SUCCESS;
}

WritablePhysicalDrive::~WritablePhysicalDrive()
{
    release_volume_locks();
    close_device();
}

bool WritablePhysicalDrive::is_open() const noexcept
{
    return ready_ && handle_ != INVALID_HANDLE_VALUE && size_ != 0;
}

DWORD WritablePhysicalDrive::open_error() const noexcept
{
    return open_error_;
}

const std::string& WritablePhysicalDrive::admission_error() const noexcept
{
    return admission_error_;
}

unsigned WritablePhysicalDrive::index() const noexcept
{
    return index_;
}

std::uint32_t WritablePhysicalDrive::logical_sector_size() const noexcept
{
    return logical_sector_size_;
}

std::size_t WritablePhysicalDrive::locked_volume_count() const noexcept
{
    return locked_volumes_.size();
}

std::uint64_t WritablePhysicalDrive::size_bytes() const
{
    return size_;
}

std::string WritablePhysicalDrive::display_name() const
{
    return "\\\\.\\PhysicalDrive" + std::to_string(index_);
}

bool WritablePhysicalDrive::read(std::uint64_t offset, std::span<std::byte> out)
{
    if (!ready_ || handle_ == INVALID_HANDLE_VALUE ||
        offset > size_ || out.size() > size_ - offset) {
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
        DWORD transferred = 0;
        if (!finish_overlapped(handle_, overlapped, started, transferred) ||
            transferred == 0) {
            return false;
        }
        done += transferred;
    }
    return true;
}

bool WritablePhysicalDrive::write(std::uint64_t offset, std::span<const std::byte> in)
{
    if (!is_open() || offset > size_ || in.size() > size_ - offset) {
        return false;
    }
    if (in.empty()) {
        return true;
    }
    if ((offset % logical_sector_size_) != 0 ||
        (in.size() % logical_sector_size_) != 0) {
        return false;
    }

    ScopedEvent event;
    if (!event.valid()) {
        return false;
    }

    constexpr std::size_t max_chunk =
        static_cast<std::size_t>(std::numeric_limits<DWORD>::max()) & ~std::size_t{511};
    std::size_t done = 0;
    while (done < in.size()) {
        const auto remaining = in.size() - done;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, max_chunk));
        if (chunk == 0 || !ResetEvent(event.get())) {
            return false;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        set_overlapped_offset(overlapped, offset + done);
        const BOOL started = WriteFile(handle_, in.data() + done, chunk, nullptr, &overlapped);
        DWORD transferred = 0;
        if (!finish_overlapped(handle_, overlapped, started, transferred) ||
            transferred != chunk) {
            return false;
        }
        done += transferred;
    }
    return true;
}

bool WritablePhysicalDrive::flush()
{
    return is_open() && FlushFileBuffers(handle_) != FALSE;
}

bool WritablePhysicalDrive::lock_target_volumes(bool dismount)
{
    wchar_t volume_name[MAX_PATH]{};
    HANDLE finder = FindFirstVolumeW(volume_name, static_cast<DWORD>(std::size(volume_name)));
    if (finder == INVALID_HANDLE_VALUE) {
        open_error_ = GetLastError();
        admission_error_ = "could not enumerate Windows volumes before physical write";
        return false;
    }

    bool ok = true;
    do {
        const auto open_path = volume_open_path(volume_name);
        HANDLE probe = CreateFileW(open_path.c_str(), 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            open_error_ = GetLastError();
            admission_error_ = "could not inspect a Windows volume while acquiring the physical write lease";
            ok = false;
            break;
        }

        bool uses_target = false;
        const bool queried = volume_uses_disk(probe, index_, uses_target);
        CloseHandle(probe);
        if (!queried) {
            open_error_ = ERROR_INVALID_DATA;
            admission_error_ = "could not map a Windows volume to its backing physical disk";
            ok = false;
            break;
        }
        if (!uses_target) {
            continue;
        }

        HANDLE volume = CreateFileW(open_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (volume == INVALID_HANDLE_VALUE) {
            open_error_ = GetLastError();
            admission_error_ = "a target Windows volume could not be opened for locking";
            ok = false;
            break;
        }

        DWORD ignored = 0;
        if (!DeviceIoControl(volume, FSCTL_LOCK_VOLUME,
                             nullptr, 0, nullptr, 0, &ignored, nullptr)) {
            open_error_ = GetLastError();
            admission_error_ = "a mounted volume on the target disk refused FSCTL_LOCK_VOLUME";
            CloseHandle(volume);
            ok = false;
            break;
        }
        if (dismount && !DeviceIoControl(volume, FSCTL_DISMOUNT_VOLUME,
                                         nullptr, 0, nullptr, 0, &ignored, nullptr)) {
            open_error_ = GetLastError();
            admission_error_ = "a locked target volume refused FSCTL_DISMOUNT_VOLUME";
            DeviceIoControl(volume, FSCTL_UNLOCK_VOLUME,
                            nullptr, 0, nullptr, 0, &ignored, nullptr);
            CloseHandle(volume);
            ok = false;
            break;
        }

        locked_volumes_.push_back(volume);
    } while (FindNextVolumeW(finder, volume_name, static_cast<DWORD>(std::size(volume_name))));

    const DWORD enumeration_error = GetLastError();
    FindVolumeClose(finder);
    if (ok && enumeration_error != ERROR_NO_MORE_FILES) {
        open_error_ = enumeration_error;
        admission_error_ = "Windows volume enumeration ended unexpectedly";
        ok = false;
    }

    if (!ok) {
        release_volume_locks();
    }
    return ok;
}

void WritablePhysicalDrive::release_volume_locks() noexcept
{
    for (auto it = locked_volumes_.rbegin(); it != locked_volumes_.rend(); ++it) {
        DWORD ignored = 0;
        DeviceIoControl(*it, FSCTL_UNLOCK_VOLUME,
                        nullptr, 0, nullptr, 0, &ignored, nullptr);
        CloseHandle(*it);
    }
    locked_volumes_.clear();
}

void WritablePhysicalDrive::close_device() noexcept
{
    ready_ = false;
    if (handle_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(handle_, nullptr);
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

} // namespace ps2hdd

#endif
