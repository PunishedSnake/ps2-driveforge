#pragma once

#include "ps2hdd/block_device.hpp"

#include <filesystem>
#include <memory>

namespace ps2hdd {

class FileBlockDevice final : public BlockDevice {
public:
    explicit FileBlockDevice(std::filesystem::path path);
    ~FileBlockDevice() override;

    FileBlockDevice(const FileBlockDevice&) = delete;
    FileBlockDevice& operator=(const FileBlockDevice&) = delete;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint64_t size_bytes() const override;
    [[nodiscard]] std::string display_name() const override;
    bool read(std::uint64_t offset, std::span<std::byte> out) override;

private:
    struct Impl;

    std::filesystem::path path_;
    std::unique_ptr<Impl> impl_;
    std::uint64_t size_{};
};

} // namespace ps2hdd
