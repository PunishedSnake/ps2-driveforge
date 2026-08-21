#pragma once

#include "ps2hdd/block_device.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>

namespace ps2hdd {

struct ReadCacheStats {
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t evictions{};
    std::uint64_t bypass_reads{};
    std::uint64_t bytes_served{};
    std::uint64_t fill_bytes{};
};

// A deliberately small, platform-neutral first-level cache for metadata-shaped
// reads. Requests that fit inside one 4 KiB page are cached; larger/cross-page
// requests bypass directly to the inner device so Emilia does not fragment the
// existing 64 KiB sequential PFS path into tiny backend calls.
class ReadCacheBlockDevice final : public BlockDevice {
public:
    static constexpr std::size_t kPageSize = 4096;
    static constexpr std::size_t kMaxPages = 4096; // 16 MiB maximum payload.

    explicit ReadCacheBlockDevice(BlockDevice& inner) : inner_(inner) {}

    [[nodiscard]] std::uint64_t size_bytes() const override { return inner_.size_bytes(); }
    [[nodiscard]] std::string display_name() const override { return inner_.display_name(); }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        const auto size = size_bytes();
        if (offset > size || out.size() > size - offset) {
            return false;
        }
        if (out.empty()) {
            return true;
        }

        const auto page_start = offset - (offset % kPageSize);
        const auto last_byte = offset + out.size() - 1;
        if (out.size() > kPageSize || last_byte / kPageSize != page_start / kPageSize) {
            bypass_reads_.fetch_add(1, std::memory_order_relaxed);
            return inner_.read(offset, out);
        }

        const auto page_index = page_start / kPageSize;
        const auto in_page = static_cast<std::size_t>(offset - page_start);
        {
            std::shared_lock lock(mutex_);
            const auto found = pages_.find(page_index);
            if (found != pages_.end() && in_page + out.size() <= found->second.valid) {
                std::memcpy(out.data(), found->second.bytes.data() + in_page, out.size());
                hits_.fetch_add(1, std::memory_order_relaxed);
                bytes_served_.fetch_add(out.size(), std::memory_order_relaxed);
                return true;
            }
        }

        misses_.fetch_add(1, std::memory_order_relaxed);
        Page loaded{};
        loaded.valid = static_cast<std::size_t>(
            std::min<std::uint64_t>(kPageSize, size - page_start));
        if (!inner_.read(page_start, std::span<std::byte>(loaded.bytes).first(loaded.valid))) {
            return false;
        }
        fill_bytes_.fetch_add(loaded.valid, std::memory_order_relaxed);

        if (in_page + out.size() > loaded.valid) {
            return false;
        }
        std::memcpy(out.data(), loaded.bytes.data() + in_page, out.size());
        bytes_served_.fetch_add(out.size(), std::memory_order_relaxed);

        {
            std::unique_lock lock(mutex_);
            if (!pages_.contains(page_index) && pages_.size() >= kMaxPages && !pages_.empty()) {
                pages_.erase(pages_.begin());
                evictions_.fetch_add(1, std::memory_order_relaxed);
            }
            pages_.insert_or_assign(page_index, std::move(loaded));
        }
        return true;
    }

    [[nodiscard]] ReadCacheStats stats() const noexcept
    {
        return {
            hits_.load(std::memory_order_relaxed),
            misses_.load(std::memory_order_relaxed),
            evictions_.load(std::memory_order_relaxed),
            bypass_reads_.load(std::memory_order_relaxed),
            bytes_served_.load(std::memory_order_relaxed),
            fill_bytes_.load(std::memory_order_relaxed),
        };
    }

    void reset_stats() noexcept
    {
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
        evictions_.store(0, std::memory_order_relaxed);
        bypass_reads_.store(0, std::memory_order_relaxed);
        bytes_served_.store(0, std::memory_order_relaxed);
        fill_bytes_.store(0, std::memory_order_relaxed);
    }

    void clear()
    {
        std::unique_lock lock(mutex_);
        pages_.clear();
    }

private:
    struct Page {
        std::array<std::byte, kPageSize> bytes{};
        std::size_t valid{};
    };

    BlockDevice& inner_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::uint64_t, Page> pages_;
    std::atomic<std::uint64_t> hits_{};
    std::atomic<std::uint64_t> misses_{};
    std::atomic<std::uint64_t> evictions_{};
    std::atomic<std::uint64_t> bypass_reads_{};
    std::atomic<std::uint64_t> bytes_served_{};
    std::atomic<std::uint64_t> fill_bytes_{};
};

} // namespace ps2hdd
