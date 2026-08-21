#pragma once

#ifdef _WIN32

#include "ps2hdd/block_device.hpp"

#include <memory>
#include <string>

namespace ps2hdd {

class DokanyMountController {
public:
    DokanyMountController();
    ~DokanyMountController();

    DokanyMountController(const DokanyMountController&) = delete;
    DokanyMountController& operator=(const DokanyMountController&) = delete;

    // Start Dokany on a worker thread. Ownership of the read-only source moves
    // into the controller and remains alive until the filesystem is unmounted.
    [[nodiscard]] bool start(std::unique_ptr<BlockDevice> source,
                             std::wstring mount_point,
                             bool debug = false);
    [[nodiscard]] bool request_unmount();
    void wait();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_mounted() const noexcept;
    [[nodiscard]] int last_status() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] std::wstring mount_point() const;

    [[nodiscard]] static std::wstring normalize_mount_point(std::wstring value);
    [[nodiscard]] static std::wstring suggest_mount_point(wchar_t preferred = L'P');

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Standalone helper used by the diagnostic CLI when it is asked to remove a
// mount created by another process.
[[nodiscard]] bool remove_dokany_mount_point(std::wstring mount_point);

} // namespace ps2hdd

#endif
