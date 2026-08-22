#pragma once

#include "ps2hdd/writable_block_device.hpp"

#include <filesystem>
#include <memory>

namespace ps2hdd {

// Existing-file, fixed-size writable image backend used by the first Frieren
// mutation work. It never creates, truncates, or extends an image.
class WritableFileBlockDevice final : public WritableBlockDevice {
public:
    explicit WritableFileBlockDevice(std::filesystem::path path);
    ~WritableFileBlockDevice() override;

    WritableFileBlockDevice(const WritableFileBlockDevice&) = delete;
    WritableFileBlockDevice& operator=(const WritableFileBlockDevice&) = delete;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint64_t size_bytes() const override;
    [[nodiscard]] std::string display_name() const override;
    bool read(std::uint64_t offset, std::span<std::byte> out) override;
    bool write(std::uint64_t offset, std::span<const std::byte> in) override;
    bool flush() override;

private:
    struct Impl;

    std::filesystem::path path_;
    std::unique_ptr<Impl> impl_;
    std::uint64_t size_{};
};

} // namespace ps2hdd
