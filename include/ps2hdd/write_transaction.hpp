#pragma once

#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd {

inline constexpr std::uint64_t kMutationSectorSize = 512;

struct StagedWrite {
    std::uint64_t offset{};
    std::vector<std::byte> before;
    std::vector<std::byte> after;
    std::string label;
};

struct WriteTransactionResult {
    bool ok{};
    bool rollback_attempted{};
    bool rollback_ok{};
    std::size_t staged_ranges{};
    std::size_t writes_started{};
    std::string error;
};

// Small metadata transaction primitive. Every replacement range is staged with
// its exact before-image before the first mutation. Ranges are 512-byte-sector
// aligned, non-overlapping and fixed within the existing backing store.
//
// This class deliberately knows nothing about APA, PFS, HDL or bootstrap rules.
// Format code decides which bytes are legal; WriteTransaction owns only capture
// -> write -> flush -> exact readback -> optional parser verification ->
// rollback. A generic byte transaction is useful precisely because it does not
// develop opinions about filesystems behind everybody's back.
class WriteTransaction final {
public:
    using Verifier = std::function<std::string()>;

    explicit WriteTransaction(WritableBlockDevice& device) : device_(device) {}

    [[nodiscard]] bool stage(std::uint64_t offset,
                             std::span<const std::byte> replacement,
                             std::string_view label = {});

    [[nodiscard]] const std::string& stage_error() const noexcept { return stage_error_; }
    [[nodiscard]] const std::vector<StagedWrite>& staged_writes() const noexcept
    {
        return writes_;
    }

    // Verifier returns an empty string on success or a human-readable failure.
    // It runs only after byte-for-byte readback succeeds. Any post-write failure
    // triggers restoration of all staged before-images. This rollback is the
    // in-process transaction mechanism; persistent crash recovery, when needed,
    // belongs to Mutation Journal rather than being smuggled into this class.
    [[nodiscard]] WriteTransactionResult commit(Verifier verifier = {});

private:
    [[nodiscard]] bool rollback() noexcept;
    [[nodiscard]] bool ranges_overlap(std::uint64_t offset, std::uint64_t bytes) const noexcept;

    WritableBlockDevice& device_;
    std::vector<StagedWrite> writes_;
    std::string stage_error_;
    bool commit_started_{};
};

} // namespace ps2hdd
