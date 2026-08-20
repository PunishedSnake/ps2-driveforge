#pragma once

#include "ps2hdd/block_device.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace ps2hdd {

class FileBlockDevice final : public BlockDevice {
public:
    explicit FileBlockDevice(std::filesystem::path path);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint64_t size_bytes() const override;
    [[nodiscard]] std::string display_name() const override;
    bool read(std::uint64_t offset, std::span<std::byte> out) override;

private:
    std::filesystem::path path_;
    std::ifstream stream_;
    std::uint64_t size_{};
    mutable std::mutex mutex_;
};

} // namespace ps2hdd
