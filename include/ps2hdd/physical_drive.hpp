#pragma once

#include "ps2hdd/block_device.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace ps2hdd {

class PhysicalDrive final : public BlockDevice {
public:
    explicit PhysicalDrive(unsigned index);
    ~PhysicalDrive() override;

    PhysicalDrive(const PhysicalDrive&) = delete;
    PhysicalDrive& operator=(const PhysicalDrive&) = delete;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] DWORD open_error() const noexcept;
    [[nodiscard]] unsigned index() const noexcept;
    [[nodiscard]] std::uint64_t size_bytes() const override;
    [[nodiscard]] std::string display_name() const override;
    [[nodiscard]] StorageCharacteristics storage_characteristics() const noexcept override
    {
        return characteristics_;
    }
    bool read(std::uint64_t offset, std::span<std::byte> out) override;

private:
    unsigned index_{};
    HANDLE handle_{INVALID_HANDLE_VALUE};
    DWORD open_error_{ERROR_SUCCESS};
    std::uint64_t size_{};
    StorageCharacteristics characteristics_{};
};

} // namespace ps2hdd
#endif
