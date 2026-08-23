#include "ps2hdd/write_transaction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class MemoryWritableDevice final : public ps2hdd::WritableBlockDevice {
public:
    explicit MemoryWritableDevice(std::size_t size) : bytes_(size) {}

    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "transaction-memory"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - offset) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        ++write_calls;
        if (offset > bytes_.size() || in.size() > bytes_.size() - offset) {
            return false;
        }
        std::copy(in.begin(), in.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        if (corrupt_write_call != 0 && write_calls == corrupt_write_call && !in.empty()) {
            bytes_[static_cast<std::size_t>(offset)] ^= std::byte{0x40};
        }
        return true;
    }

    bool flush() override
    {
        ++flush_calls;
        return !fail_flush;
    }

    std::span<std::byte> bytes() { return bytes_; }
    std::span<const std::byte> bytes() const { return bytes_; }

    std::size_t write_calls{};
    std::size_t flush_calls{};
    std::size_t corrupt_write_call{};
    bool fail_flush{};

private:
    std::vector<std::byte> bytes_;
};

std::vector<std::byte> filled(std::size_t size, unsigned value)
{
    return std::vector<std::byte>(size, static_cast<std::byte>(value));
}

void test_two_range_commit()
{
    MemoryWritableDevice device(4096);
    const auto a = filled(512, 0x11);
    const auto b = filled(1024, 0x22);

    ps2hdd::WriteTransaction transaction(device);
    check(transaction.stage(512, a, "first"), "first stage failed");
    check(transaction.stage(2048, b, "second"), "second stage failed");

    const auto result = transaction.commit([] { return std::string{}; });
    check(result.ok, "valid transaction should commit");
    check(!result.rollback_attempted, "successful transaction must not roll back");
    check(device.flush_calls == 1, "successful multi-range transaction should flush once");
    check(std::equal(a.begin(), a.end(), device.bytes().begin() + 512), "first range mismatch");
    check(std::equal(b.begin(), b.end(), device.bytes().begin() + 2048), "second range mismatch");
}

void test_alignment_overlap_and_bounds_refusal()
{
    MemoryWritableDevice device(4096);
    const auto sector = filled(512, 0x31);
    const auto two_sectors = filled(1024, 0x32);

    ps2hdd::WriteTransaction transaction(device);
    check(!transaction.stage(1, sector), "unaligned offset should be refused");
    check(!transaction.stage(0, std::span<const std::byte>(sector).first(511)),
          "partial sector should be refused");
    check(transaction.stage(512, two_sectors), "valid stage failed");
    check(!transaction.stage(1024, sector), "overlapping stage should be refused");
    check(!transaction.stage(4096, sector), "out-of-bounds stage should be refused");
    check(device.write_calls == 0, "staging refusal must not mutate the device");
}

void test_corrupted_readback_restores_every_before_image()
{
    MemoryWritableDevice device(4096);
    std::fill(device.bytes().begin(), device.bytes().end(), std::byte{0x5A});
    const auto original = std::vector<std::byte>(device.bytes().begin(), device.bytes().end());

    const auto a = filled(512, 0x61);
    const auto b = filled(512, 0x62);
    ps2hdd::WriteTransaction transaction(device);
    check(transaction.stage(512, a), "first stage failed");
    check(transaction.stage(1536, b), "second stage failed");

    // Corrupt the second forward write. Rollback writes are calls 3 and 4.
    device.corrupt_write_call = 2;
    const auto result = transaction.commit();
    check(!result.ok, "corrupted readback must fail commit");
    check(result.rollback_attempted && result.rollback_ok,
          "corrupted transaction should restore all before-images");
    check(device.write_calls == 4, "two forward writes plus two rollback writes expected");
    check(device.flush_calls == 2, "forward commit and rollback should each flush once");
    check(std::equal(original.begin(), original.end(), device.bytes().begin()),
          "rollback did not restore original device bytes");
}

void test_parser_verifier_failure_rolls_back()
{
    MemoryWritableDevice device(2048);
    std::fill(device.bytes().begin(), device.bytes().end(), std::byte{0x7B});
    const auto original = std::vector<std::byte>(device.bytes().begin(), device.bytes().end());
    const auto replacement = filled(512, 0x44);

    ps2hdd::WriteTransaction transaction(device);
    check(transaction.stage(512, replacement), "stage failed");
    const auto result = transaction.commit([] { return std::string{"synthetic parser rejection"}; });

    check(!result.ok, "verifier rejection must fail commit");
    check(result.error.find("synthetic parser rejection") != std::string::npos,
          "verifier error should be preserved");
    check(result.rollback_ok, "verifier rejection should roll back successfully");
    check(std::equal(original.begin(), original.end(), device.bytes().begin()),
          "verifier rollback did not restore original bytes");
}

} // namespace

int main()
{
    try {
        test_two_range_commit();
        test_alignment_overlap_and_bounds_refusal();
        test_corrupted_readback_restores_every_before_image();
        test_parser_verifier_failure_rolls_back();
        std::cout << "Write transaction tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Write transaction tests failed: " << error.what() << '\n';
        return 1;
    }
}
