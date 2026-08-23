#pragma once

#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/writable_block_device.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd {

struct WritablePhysicalDriveOptions {
    physical_write::Authorization authorization;
    bool lock_mounted_volumes{true};
    bool dismount_locked_volumes{true};
};

// Separate raw-disk write capability. The ordinary PhysicalDrive class remains
// GENERIC_READ forever; code must ask for this much louder type on purpose.
// Opening it is a two-stage gate: verify the read-only authorization, lock any
// Windows volumes backed by the target disk, then verify the authorization
// again. A drive number alone never authorizes mutation because Windows enjoys
// renumbering hardware whenever that would be least convenient.
class WritablePhysicalDrive final : public WritableBlockDevice {
public:
    WritablePhysicalDrive(unsigned index, const WritablePhysicalDriveOptions& options);
    ~WritablePhysicalDrive() override;

    WritablePhysicalDrive(const WritablePhysicalDrive&) = delete;
    WritablePhysicalDrive& operator=(const WritablePhysicalDrive&) = delete;

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] DWORD open_error() const noexcept;
    [[nodiscard]] const std::string& admission_error() const noexcept;
    [[nodiscard]] unsigned index() const noexcept;
    [[nodiscard]] std::uint32_t logical_sector_size() const noexcept;
    [[nodiscard]] std::size_t locked_volume_count() const noexcept;

    [[nodiscard]] std::uint64_t size_bytes() const override;
    [[nodiscard]] std::string display_name() const override;
    bool read(std::uint64_t offset, std::span<std::byte> out) override;
    bool write(std::uint64_t offset, std::span<const std::byte> in) override;
    bool flush() override;

private:
    bool lock_target_volumes(bool dismount);
    void release_volume_locks() noexcept;
    void close_device() noexcept;

    unsigned index_{};
    HANDLE handle_{INVALID_HANDLE_VALUE};
    DWORD open_error_{ERROR_SUCCESS};
    std::string admission_error_;
    std::uint64_t size_{};
    std::uint32_t logical_sector_size_{};
    bool ready_{};
    std::vector<HANDLE> locked_volumes_;
};

} // namespace ps2hdd
#endif
