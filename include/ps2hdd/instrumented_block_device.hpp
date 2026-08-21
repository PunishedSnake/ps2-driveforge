#pragma once

#include "ps2hdd/block_device.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>
#include <string>

namespace ps2hdd {

struct BlockIoStats {
    std::uint64_t read_calls{};
    std::uint64_t bytes_requested{};
    std::uint64_t failed_reads{};
    std::uint64_t largest_read{};
};

// Transparent read-only BlockDevice wrapper used for diagnostics/benchmarks.
// Keeping counters at the BlockDevice boundary measures actual host I/O requests
// after PFS batching and APA translation, rather than only logical file bytes.
class InstrumentedBlockDevice final : public BlockDevice {
public:
    explicit InstrumentedBlockDevice(BlockDevice& inner) : inner_(inner) {}

    [[nodiscard]] std::uint64_t size_bytes() const override { return inner_.size_bytes(); }
    [[nodiscard]] std::string display_name() const override { return inner_.display_name(); }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        read_calls_.fetch_add(1, std::memory_order_relaxed);
        bytes_requested_.fetch_add(out.size(), std::memory_order_relaxed);

        auto observed = largest_read_.load(std::memory_order_relaxed);
        while (observed < out.size() &&
               !largest_read_.compare_exchange_weak(observed, out.size(),
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
        }

        const bool ok = inner_.read(offset, out);
        if (!ok) {
            failed_reads_.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    }

    [[nodiscard]] BlockIoStats stats() const noexcept
    {
        return {
            read_calls_.load(std::memory_order_relaxed),
            bytes_requested_.load(std::memory_order_relaxed),
            failed_reads_.load(std::memory_order_relaxed),
            largest_read_.load(std::memory_order_relaxed),
        };
    }

    void reset_stats() noexcept
    {
        read_calls_.store(0, std::memory_order_relaxed);
        bytes_requested_.store(0, std::memory_order_relaxed);
        failed_reads_.store(0, std::memory_order_relaxed);
        largest_read_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] BlockDevice& inner() noexcept { return inner_; }

private:
    BlockDevice& inner_;
    std::atomic<std::uint64_t> read_calls_{};
    std::atomic<std::uint64_t> bytes_requested_{};
    std::atomic<std::uint64_t> failed_reads_{};
    std::atomic<std::uint64_t> largest_read_{};
};

} // namespace ps2hdd
