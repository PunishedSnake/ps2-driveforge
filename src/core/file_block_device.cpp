#include "ps2hdd/file_block_device.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ps2hdd {

struct FileBlockDevice::Impl {
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int fd{-1};
#endif
};

#ifdef _WIN32
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
#endif

FileBlockDevice::FileBlockDevice(std::filesystem::path path)
    : path_(std::move(path)), impl_(std::make_unique<Impl>())
{
#ifdef _WIN32
    impl_->handle = CreateFileW(path_.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
                                nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER length{};
    if (!GetFileSizeEx(impl_->handle, &length) || length.QuadPart < 0) {
        CloseHandle(impl_->handle);
        impl_->handle = INVALID_HANDLE_VALUE;
        return;
    }
    size_ = static_cast<std::uint64_t>(length.QuadPart);
#else
    impl_->fd = ::open(path_.c_str(), O_RDONLY);
    if (impl_->fd < 0) {
        return;
    }

    struct stat info {};
    if (::fstat(impl_->fd, &info) != 0 || info.st_size < 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
        return;
    }
    size_ = static_cast<std::uint64_t>(info.st_size);
#endif
}

FileBlockDevice::~FileBlockDevice()
{
    if (!impl_) {
        return;
    }
#ifdef _WIN32
    if (impl_->handle != INVALID_HANDLE_VALUE) {
        CancelIoEx(impl_->handle, nullptr);
        CloseHandle(impl_->handle);
    }
#else
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
    }
#endif
}

bool FileBlockDevice::is_open() const noexcept
{
    if (!impl_) {
        return false;
    }
#ifdef _WIN32
    return impl_->handle != INVALID_HANDLE_VALUE;
#else
    return impl_->fd >= 0;
#endif
}

std::uint64_t FileBlockDevice::size_bytes() const
{
    return size_;
}

std::string FileBlockDevice::display_name() const
{
    return path_.string();
}

bool FileBlockDevice::read(std::uint64_t offset, std::span<std::byte> out)
{
    if (!is_open() || offset > size_ || out.size() > size_ - offset) {
        return false;
    }
    if (out.empty()) {
        return true;
    }

#ifdef _WIN32
    ScopedEvent event;
    if (!event.valid()) {
        return false;
    }

    std::size_t done = 0;
    while (done < out.size()) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(out.size() - done, std::numeric_limits<DWORD>::max()));
        if (!ResetEvent(event.get())) {
            return false;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        set_overlapped_offset(overlapped, offset + done);
        const BOOL started = ReadFile(impl_->handle, out.data() + done, chunk, nullptr, &overlapped);
        DWORD transferred = 0;
        if (!finish_overlapped(impl_->handle, overlapped, started, transferred) || transferred == 0) {
            return false;
        }
        done += transferred;
    }
    return true;
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return false;
    }

    std::size_t done = 0;
    while (done < out.size()) {
        const auto absolute = offset + done;
        if (absolute > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            return false;
        }
        const auto chunk = std::min<std::size_t>(
            out.size() - done, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t transferred = ::pread(impl_->fd, out.data() + done, chunk,
                                            static_cast<off_t>(absolute));
        if (transferred <= 0) {
            return false;
        }
        done += static_cast<std::size_t>(transferred);
    }
    return true;
#endif
}

} // namespace ps2hdd
