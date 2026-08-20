#include "ps2hdd/file_block_device.hpp"

#include <system_error>
#include <utility>

namespace ps2hdd {

FileBlockDevice::FileBlockDevice(std::filesystem::path path)
    : path_(std::move(path)), stream_(path_, std::ios::binary)
{
    std::error_code ec;
    size_ = std::filesystem::file_size(path_, ec);
    if (ec) {
        size_ = 0;
    }
}

bool FileBlockDevice::is_open() const noexcept
{
    return stream_.is_open();
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
    if (!stream_.is_open() || offset > size_ || out.size() > size_ - offset) {
        return false;
    }

    std::scoped_lock lock(mutex_);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) {
        return false;
    }

    stream_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return stream_.gcount() == static_cast<std::streamsize>(out.size());
}

} // namespace ps2hdd
