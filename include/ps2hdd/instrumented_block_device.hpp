#pragma once

#include "ps2hdd/block_device.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace ps2hdd {

struct BlockIoStats {
    std::uint64_t read_calls{};
    std::uint64_t bytes_requested{};
    std::uint64_t failed_reads{};
    std::uint64_t largest_read{};
    std::uint64_t small_read_calls{};
    std::uint64_t read_time_ns{};
    std::uint64_t max_in_flight{};
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
        if (out.size() <= 4096) {
            small_read_calls_.fetch_add(1, std::memory_order_relaxed);
        }

        auto observed = largest_read_.load(std::memory_order_relaxed);
        while (observed < out.size() &&
               !largest_read_.compare_exchange_weak(observed, out.size(),
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
        }

        const auto active = in_flight_.fetch_add(1, std::memory_order_relaxed) + 1;
        auto max_active = max_in_flight_.load(std::memory_order_relaxed);
        while (max_active < active &&
               !max_in_flight_.compare_exchange_weak(max_active, active,
                                                     std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {
        }

        const auto started = std::chrono::steady_clock::now();
        const bool ok = inner_.read(offset, out);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count();
        read_time_ns_.fetch_add(static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0)),
                                std::memory_order_relaxed);
        in_flight_.fetch_sub(1, std::memory_order_relaxed);

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
            small_read_calls_.load(std::memory_order_relaxed),
            read_time_ns_.load(std::memory_order_relaxed),
            max_in_flight_.load(std::memory_order_relaxed),
        };
    }

    void reset_stats() noexcept
    {
        read_calls_.store(0, std::memory_order_relaxed);
        bytes_requested_.store(0, std::memory_order_relaxed);
        failed_reads_.store(0, std::memory_order_relaxed);
        largest_read_.store(0, std::memory_order_relaxed);
        small_read_calls_.store(0, std::memory_order_relaxed);
        read_time_ns_.store(0, std::memory_order_relaxed);
        max_in_flight_.store(in_flight_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    [[nodiscard]] BlockDevice& inner() noexcept { return inner_; }

private:
    BlockDevice& inner_;
    std::atomic<std::uint64_t> read_calls_{};
    std::atomic<std::uint64_t> bytes_requested_{};
    std::atomic<std::uint64_t> failed_reads_{};
    std::atomic<std::uint64_t> largest_read_{};
    std::atomic<std::uint64_t> small_read_calls_{};
    std::atomic<std::uint64_t> read_time_ns_{};
    std::atomic<std::uint64_t> in_flight_{};
    std::atomic<std::uint64_t> max_in_flight_{};
};

} // namespace ps2hdd
