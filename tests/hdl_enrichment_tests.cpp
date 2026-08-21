#include "ps2hdd/hdl_enrichment.hpp"
#include "ps2hdd/apa.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

ps2hdd::apa::Partition make_hdl(std::string id, std::uint32_t start)
{
    ps2hdd::apa::Partition partition;
    partition.id = std::move(id);
    partition.type = ps2hdd::apa::kTypeHdl;
    partition.start_lba = start;
    partition.length_sectors = 0x5000;
    partition.total_sectors = partition.length_sectors;
    return partition;
}

void store_u32(std::byte* p, std::uint32_t value)
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

class ProfiledMemoryDevice final : public ps2hdd::BlockDevice {
public:
    ProfiledMemoryDevice(std::size_t size, ps2hdd::StorageMediaClass media)
        : bytes_(size)
    {
        storage_.media_class = media;
        storage_.seek_penalty_known = media != ps2hdd::StorageMediaClass::unknown;
        storage_.incurs_seek_penalty = media == ps2hdd::StorageMediaClass::rotational;
    }

    [[nodiscard]] std::uint64_t size_bytes() const override { return bytes_.size(); }
    [[nodiscard]] std::string display_name() const override { return "scheduler fixture"; }
    [[nodiscard]] ps2hdd::StorageCharacteristics storage_characteristics() const noexcept override
    {
        return storage_;
    }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }

        const auto active = in_flight_.fetch_add(1, std::memory_order_relaxed) + 1;
        auto observed = max_in_flight_.load(std::memory_order_relaxed);
        while (observed < active &&
               !max_in_flight_.compare_exchange_weak(observed, active,
                                                     std::memory_order_relaxed,
                                                     std::memory_order_relaxed)) {
        }

        // Make independent reads overlap reliably in the SSD scheduler test.
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        std::memcpy(out.data(), bytes_.data() + static_cast<std::size_t>(offset), out.size());
        {
            std::scoped_lock lock(offsets_mutex_);
            offsets_.push_back(offset);
        }
        in_flight_.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    void write(std::uint64_t offset, std::span<const std::byte> data)
    {
        if (offset > bytes_.size() || data.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            throw std::runtime_error("fixture write out of range");
        }
        std::memcpy(bytes_.data() + static_cast<std::size_t>(offset), data.data(), data.size());
    }

    [[nodiscard]] std::uint64_t max_in_flight() const noexcept
    {
        return max_in_flight_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::vector<std::uint64_t> offsets() const
    {
        std::scoped_lock lock(offsets_mutex_);
        return offsets_;
    }

private:
    std::vector<std::byte> bytes_;
    ps2hdd::StorageCharacteristics storage_{};
    std::atomic<std::uint64_t> in_flight_{};
    std::atomic<std::uint64_t> max_in_flight_{};
    mutable std::mutex offsets_mutex_;
    std::vector<std::uint64_t> offsets_;
};

void write_header(ProfiledMemoryDevice& device, const ps2hdd::apa::Partition& partition,
                  std::string_view title)
{
    std::array<std::byte, ps2hdd::hdl::kMetadataBytes> header{};
    store_u32(header.data(), ps2hdd::hdl::kMagic);
    std::memcpy(header.data() + ps2hdd::hdl::kTitleOffset, title.data(), title.size());
    const auto offset = static_cast<std::uint64_t>(partition.start_lba) * ps2hdd::apa::kSectorSize +
                        ps2hdd::hdl::kMetadataOffset;
    device.write(offset, header);
}

ps2hdd::apa::ScanResult make_scan(ProfiledMemoryDevice& device)
{
    ps2hdd::apa::ScanResult scan;
    scan.mbr_valid = true;
    scan.apa_version = 2;

    auto fourth = make_hdl("PP.GAME4", 0x7000);
    auto second = make_hdl("PP.GAME2", 0x3000);
    auto first = make_hdl("PP.GAME1", 0x1000);
    auto third = make_hdl("PP.GAME3", 0x5000);
    scan.partitions = {fourth, second, first, third};

    write_header(device, fourth, "Game Four");
    write_header(device, second, "Game Two");
    write_header(device, first, "Game One");
    write_header(device, third, "Game Three");
    return scan;
}

void policy_contract()
{
    ps2hdd::StorageCharacteristics unknown{};
    auto policy = ps2hdd::choose_hdl_enrichment_policy(unknown, 10);
    check(policy.max_in_flight == 1 && policy.physical_lba_order,
          "unknown media starts conservatively at QD1 in physical order");

    ps2hdd::StorageCharacteristics rotational{};
    rotational.media_class = ps2hdd::StorageMediaClass::rotational;
    policy = ps2hdd::choose_hdl_enrichment_policy(rotational, 10);
    check(policy.max_in_flight == 1,
          "rotational media starts at QD1 to avoid random seek amplification");

    ps2hdd::StorageCharacteristics ssd{};
    ssd.media_class = ps2hdd::StorageMediaClass::solid_state;
    policy = ps2hdd::choose_hdl_enrichment_policy(ssd, 10);
    check(policy.max_in_flight == 4,
          "solid-state media may schedule up to four independent HDL metadata reads");

    ps2hdd::HdlEnrichmentOptions override_options;
    override_options.max_in_flight_override = 2;
    policy = ps2hdd::choose_hdl_enrichment_policy(rotational, 10, override_options);
    check(policy.max_in_flight == 2,
          "developer benchmark override is explicit and does not change automatic policy");
}

void rotational_serial_contract()
{
    ProfiledMemoryDevice device(32U * 1024U * 1024U, ps2hdd::StorageMediaClass::rotational);
    const auto scan = make_scan(device);
    const auto catalog = ps2hdd::enrich_hdl_catalog(device, scan);

    check(catalog.entries.size() == 4 && catalog.readable == 4,
          "rotational scheduler reads every HDL game in-process");
    check(device.max_in_flight() == 1,
          "rotational automatic scheduler never overlaps metadata reads");
    check(std::is_sorted(catalog.entries.begin(), catalog.entries.end(), [](const auto& a, const auto& b) {
              return a.game.start_lba < b.game.start_lba;
          }),
          "rotational result order is physical LBA order");
    const auto offsets = device.offsets();
    check(std::is_sorted(offsets.begin(), offsets.end()),
          "rotational backing reads themselves execute in ascending physical order");
}

void solid_state_parallel_contract()
{
    ProfiledMemoryDevice device(32U * 1024U * 1024U, ps2hdd::StorageMediaClass::solid_state);
    const auto scan = make_scan(device);
    std::atomic<std::size_t> progress_calls{};
    const auto catalog = ps2hdd::enrich_hdl_catalog(
        device, scan,
        [&](std::size_t, std::size_t, const ps2hdd::hdl::GameResult& result) {
            check(result.ok, "parallel progress never exposes a partially initialized result");
            progress_calls.fetch_add(1, std::memory_order_relaxed);
        });

    check(catalog.entries.size() == 4 && catalog.readable == 4,
          "solid-state scheduler returns all HDL metadata");
    check(device.max_in_flight() >= 2,
          "solid-state automatic scheduler actually overlaps independent reads");
    check(progress_calls.load(std::memory_order_relaxed) == 4,
          "parallel scheduler reports exactly one progress callback per game");
    check(std::is_sorted(catalog.entries.begin(), catalog.entries.end(), [](const auto& a, const auto& b) {
              return a.game.start_lba < b.game.start_lba;
          }),
          "parallel completion order never changes stable physical-LBA result order");
}

void cancellation_contract()
{
    ProfiledMemoryDevice device(32U * 1024U * 1024U, ps2hdd::StorageMediaClass::unknown);
    const auto scan = make_scan(device);
    std::stop_source stop;
    const auto catalog = ps2hdd::enrich_hdl_catalog(
        device, scan,
        [&](std::size_t completed, std::size_t, const ps2hdd::hdl::GameResult&) {
            if (completed == 1) {
                stop.request_stop();
            }
        },
        stop.get_token());

    check(catalog.cancelled && catalog.entries.size() == 1,
          "conservative scheduler honors cancellation before the next metadata read");
}

} // namespace

int main()
{
    try {
        policy_contract();
        rotational_serial_contract();
        solid_state_parallel_contract();
        cancellation_contract();
        std::cout << "Emilia HDL enrichment scheduler tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
