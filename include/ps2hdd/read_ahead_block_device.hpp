#pragma once

#include "ps2hdd/block_device.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd {

struct ReadAheadStats {
    std::uint64_t window_hits{};
    std::uint64_t window_misses{};
    std::uint64_t prefetches{};
    std::uint64_t prefetched_bytes{};
    std::uint64_t bytes_served_from_window{};
    std::uint64_t wasted_bytes{};
};

// Adaptive sequential read-ahead. Metadata-shaped/small requests bypass this
// layer unchanged. Two consecutive >=32 KiB reads are required before Emilia
// grows a backend read with lookahead, avoiding speculative I/O for random APA/PFS
// metadata while reducing syscall/seek churn during large Explorer copies.
class ReadAheadBlockDevice final : public BlockDevice {
public:
    static constexpr std::size_t kTriggerBytes = 32U * 1024U;
    static constexpr std::size_t kLookAheadBytes = 256U * 1024U;

    explicit ReadAheadBlockDevice(BlockDevice& inner) : inner_(inner) {}

    [[nodiscard]] std::uint64_t size_bytes() const override { return inner_.size_bytes(); }
    [[nodiscard]] std::string display_name() const override { return inner_.display_name(); }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        const auto device_size = size_bytes();
        if (offset > device_size || out.size() > device_size - offset) {
            return false;
        }
        if (out.empty()) {
            return true;
        }

        {
            std::scoped_lock lock(mutex_);
            if (!window_.empty() && offset >= window_start_) {
                const auto relative = offset - window_start_;
                if (relative <= window_.size() && out.size() <= window_.size() - relative) {
                    std::memcpy(out.data(), window_.data() + static_cast<std::size_t>(relative), out.size());
                    window_hits_.fetch_add(1, std::memory_order_relaxed);
                    bytes_served_from_window_.fetch_add(out.size(), std::memory_order_relaxed);
                    window_served_bytes_ += out.size();
                    observe_request_locked(offset, out.size());
                    return true;
                }
            }
        }

        window_misses_.fetch_add(1, std::memory_order_relaxed);
        if (out.size() < kTriggerBytes) {
            const bool ok = inner_.read(offset, out);
            if (ok) {
                std::scoped_lock lock(mutex_);
                reset_sequence_locked();
            }
            return ok;
        }

        bool should_prefetch = false;
        {
            std::scoped_lock lock(mutex_);
            should_prefetch = has_last_request_ && offset == last_request_end_;
        }

        if (!should_prefetch) {
            const bool ok = inner_.read(offset, out);
            if (ok) {
                std::scoped_lock lock(mutex_);
                observe_request_locked(offset, out.size());
            }
            return ok;
        }

        const auto available = device_size - offset;
        const auto requested_with_ahead = static_cast<std::uint64_t>(out.size()) + kLookAheadBytes;
        const auto total64 = std::min<std::uint64_t>(available, requested_with_ahead);
        const auto total = static_cast<std::size_t>(total64);
        std::vector<std::byte> loaded(total);
        if (!inner_.read(offset, loaded)) {
            return false;
        }

        std::memcpy(out.data(), loaded.data(), out.size());
        const auto ahead = total - out.size();
        prefetches_.fetch_add(1, std::memory_order_relaxed);
        prefetched_bytes_.fetch_add(ahead, std::memory_order_relaxed);

        {
            std::scoped_lock lock(mutex_);
            account_old_window_locked();
            window_start_ = offset;
            window_ = std::move(loaded);
            window_served_bytes_ = out.size();
            observe_request_locked(offset, out.size());
        }
        return true;
    }

    [[nodiscard]] ReadAheadStats stats() const noexcept
    {
        return {
            window_hits_.load(std::memory_order_relaxed),
            window_misses_.load(std::memory_order_relaxed),
            prefetches_.load(std::memory_order_relaxed),
            prefetched_bytes_.load(std::memory_order_relaxed),
            bytes_served_from_window_.load(std::memory_order_relaxed),
            wasted_bytes_.load(std::memory_order_relaxed),
        };
    }

    void reset_stats() noexcept
    {
        window_hits_.store(0, std::memory_order_relaxed);
        window_misses_.store(0, std::memory_order_relaxed);
        prefetches_.store(0, std::memory_order_relaxed);
        prefetched_bytes_.store(0, std::memory_order_relaxed);
        bytes_served_from_window_.store(0, std::memory_order_relaxed);
        wasted_bytes_.store(0, std::memory_order_relaxed);
    }

    void clear()
    {
        std::scoped_lock lock(mutex_);
        account_old_window_locked();
        window_.clear();
        window_start_ = 0;
        window_served_bytes_ = 0;
        reset_sequence_locked();
    }

private:
    void observe_request_locked(std::uint64_t offset, std::size_t size) noexcept
    {
        has_last_request_ = true;
        last_request_end_ = offset + size;
    }

    void reset_sequence_locked() noexcept
    {
        has_last_request_ = false;
        last_request_end_ = 0;
    }

    void account_old_window_locked() noexcept
    {
        if (window_.empty()) {
            return;
        }
        const auto served = std::min<std::size_t>(window_served_bytes_, window_.size());
        const auto wasted = window_.size() - served;
        wasted_bytes_.fetch_add(wasted, std::memory_order_relaxed);
    }

    BlockDevice& inner_;
    mutable std::mutex mutex_;
    std::vector<std::byte> window_;
    std::uint64_t window_start_{};
    std::size_t window_served_bytes_{};
    bool has_last_request_{};
    std::uint64_t last_request_end_{};
    std::atomic<std::uint64_t> window_hits_{};
    std::atomic<std::uint64_t> window_misses_{};
    std::atomic<std::uint64_t> prefetches_{};
    std::atomic<std::uint64_t> prefetched_bytes_{};
    std::atomic<std::uint64_t> bytes_served_from_window_{};
    std::atomic<std::uint64_t> wasted_bytes_{};
};

} // namespace ps2hdd
