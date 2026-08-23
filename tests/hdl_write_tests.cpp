#include "ps2hdd/hdl.hpp"
#include "ps2hdd/writable_file_block_device.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void store_u16_le(std::byte* p, std::uint16_t value)
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void store_u32_le(std::byte* p, std::uint32_t value)
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

class MemoryWritableDevice final : public ps2hdd::WritableBlockDevice {
public:
    explicit MemoryWritableDevice(std::size_t size) : bytes_(size) {}

    [[nodiscard]] std::uint64_t size_bytes() const override { return bytes_.size(); }
    [[nodiscard]] std::string display_name() const override { return "memory-writable"; }

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
        if (corrupt_next_write && !in.empty()) {
            corrupt_next_write = false;
            bytes_[static_cast<std::size_t>(offset) + in.size() - 1] ^= std::byte{0x01};
        }
        return true;
    }

    bool flush() override
    {
        ++flush_calls;
        return true;
    }

    std::span<std::byte> bytes() { return bytes_; }
    std::span<const std::byte> bytes() const { return bytes_; }

    std::size_t write_calls{};
    std::size_t flush_calls{};
    bool corrupt_next_write{};

private:
    std::vector<std::byte> bytes_;
};

struct TempFileGuard {
    std::filesystem::path path;

    ~TempFileGuard()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

ps2hdd::apa::Partition make_partition()
{
    ps2hdd::apa::Partition partition;
    partition.id = "PP.TEST-00001..FRIEREN";
    partition.start_lba = 2048;
    partition.length_sectors = 4096;
    partition.total_sectors = 4096;
    partition.type = ps2hdd::apa::kTypeHdl;
    partition.flags = 0;
    partition.sub_count = 0;
    return partition;
}

std::uint64_t metadata_offset(const ps2hdd::apa::Partition& partition)
{
    return static_cast<std::uint64_t>(partition.start_lba) * ps2hdd::apa::kSectorSize +
           ps2hdd::hdl::kMetadataOffset;
}

void seed_valid_metadata(MemoryWritableDevice& device,
                         const ps2hdd::apa::Partition& partition)
{
    std::array<std::byte, ps2hdd::hdl::kMetadataBytes> metadata{};
    store_u32_le(metadata.data(), ps2hdd::hdl::kMagic);

    constexpr char title[] = "Before Frieren";
    std::memcpy(metadata.data() + ps2hdd::hdl::kTitleOffset, title, sizeof(title));

    metadata[ps2hdd::hdl::kCompatOffset] = std::byte{0x02};
    store_u16_le(metadata.data() + ps2hdd::hdl::kDmaOffset, 0x0040);

    constexpr char startup[] = "SLUS_123.45";
    std::memcpy(metadata.data() + ps2hdd::hdl::kStartupOffset, startup, sizeof(startup));

    store_u32_le(metadata.data() + ps2hdd::hdl::kLayerBreakOffset, 0);
    store_u32_le(metadata.data() + ps2hdd::hdl::kMediaOffset, 0x14U);
    metadata[ps2hdd::hdl::kPartCountOffset] = std::byte{1};

    store_u32_le(metadata.data() + ps2hdd::hdl::kAllocTableOffset + 8, 0x4000U);

    auto target = device.bytes().subspan(static_cast<std::size_t>(metadata_offset(partition)),
                                         metadata.size());
    std::copy(metadata.begin(), metadata.end(), target.begin());
}

std::array<std::byte, ps2hdd::hdl::kMetadataBytes>
capture_metadata(const MemoryWritableDevice& device,
                 const ps2hdd::apa::Partition& partition)
{
    std::array<std::byte, ps2hdd::hdl::kMetadataBytes> out{};
    const auto source = device.bytes().subspan(static_cast<std::size_t>(metadata_offset(partition)),
                                               out.size());
    std::copy(source.begin(), source.end(), out.begin());
    return out;
}

void test_writable_file_backend_is_existing_and_fixed_size()
{
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("ps2-driveforge-frieren-write-" + std::to_string(unique) + ".img");
    TempFileGuard guard{path};

    std::array<char, 4096> zeros{};
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        check(output.good(), "could not create temporary writable-image fixture");
        output.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
        check(output.good(), "could not populate temporary writable-image fixture");
    }

    ps2hdd::WritableFileBlockDevice device(path);
    check(device.is_open(), "existing writable image should open");
    check(device.size_bytes() == zeros.size(), "writable image size is incorrect");

    const std::array<std::byte, 4> payload{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    check(device.write(128, payload), "bounded writable-image write failed");
    check(device.flush(), "writable-image flush failed");

    std::array<std::byte, 4> readback{};
    check(device.read(128, readback), "writable-image readback failed");
    check(readback == payload, "writable-image bytes did not round-trip");
    check(!device.write(4094, payload), "writable image must reject extending writes");
    check(device.size_bytes() == zeros.size(), "rejected write changed logical image size");
    check(std::filesystem::file_size(path) == zeros.size(), "rejected write extended image file");

    const auto missing = path.string() + ".missing";
    ps2hdd::WritableFileBlockDevice missing_device(missing);
    check(!missing_device.is_open(), "writable image backend must not create missing images");
    check(!std::filesystem::exists(missing), "writable image backend created a missing path");
}

void test_patch_round_trips_selected_fields()
{
    const auto partition = make_partition();
    MemoryWritableDevice device(4U * 1024U * 1024U);
    seed_valid_metadata(device, partition);
    const auto before = capture_metadata(device, partition);

    ps2hdd::hdl::MetadataPatch patch;
    patch.title = "Frieren Writes Safely";
    patch.compat_flags = 0x15;
    patch.dma = 0x1234;

    const auto result = ps2hdd::hdl::patch_game_metadata(device, partition, patch);
    check(result.ok, "valid HDL metadata patch should succeed");
    check(result.game.title == "Frieren Writes Safely", "title did not round-trip");
    check(result.game.compat_flags == 0x15, "compat flags did not round-trip");
    check(result.game.dma == 0x1234, "DMA did not round-trip");
    check(result.game.startup == "SLUS_123.45", "startup ID must remain unchanged");
    check(result.game.media == ps2hdd::hdl::MediaType::dvd, "media type must remain unchanged");
    check(result.game.declared_parts == 1, "allocation table count must remain unchanged");
    check(device.write_calls == 1, "successful patch should issue one metadata write");
    check(device.flush_calls == 1, "successful patch should issue one durability flush");

    const auto after = capture_metadata(device, partition);
    check(std::equal(before.begin(), before.begin() + ps2hdd::hdl::kTitleOffset,
                     after.begin()),
          "bytes before title changed unexpectedly");
    check(after[ps2hdd::hdl::kPartCountOffset] == before[ps2hdd::hdl::kPartCountOffset],
          "part count changed unexpectedly");
    check(std::equal(before.begin() + static_cast<std::ptrdiff_t>(ps2hdd::hdl::kAllocTableOffset),
                     before.end(),
                     after.begin() + static_cast<std::ptrdiff_t>(ps2hdd::hdl::kAllocTableOffset)),
          "allocation table or trailing metadata changed unexpectedly");
}

void test_patch_rejects_oversized_title_without_write()
{
    const auto partition = make_partition();
    MemoryWritableDevice device(4U * 1024U * 1024U);
    seed_valid_metadata(device, partition);

    ps2hdd::hdl::MetadataPatch patch;
    patch.title = std::string(ps2hdd::hdl::kTitleStorage, 'X');

    const auto result = ps2hdd::hdl::patch_game_metadata(device, partition, patch);
    check(!result.ok, "oversized title must be rejected");
    check(device.write_calls == 0, "rejected title must not write source bytes");
    check(device.flush_calls == 0, "rejected title must not flush source bytes");
}

void test_patch_rolls_back_failed_readback()
{
    const auto partition = make_partition();
    MemoryWritableDevice device(4U * 1024U * 1024U);
    seed_valid_metadata(device, partition);
    const auto before = capture_metadata(device, partition);

    device.corrupt_next_write = true;
    ps2hdd::hdl::MetadataPatch patch;
    patch.title = "This write will be corrupted";

    const auto result = ps2hdd::hdl::patch_game_metadata(device, partition, patch);
    check(!result.ok, "corrupted read-back must fail the patch");
    check(result.error.find("before-images restored") != std::string::npos,
          "failed read-back should report successful transaction rollback");
    check(device.write_calls == 2, "failed verification should write once and then roll back");
    check(device.flush_calls == 2, "failed verification should flush write and rollback");
    check(capture_metadata(device, partition) == before,
          "rollback must restore the complete original metadata window");
}

void test_patch_refuses_invalid_hdl_header()
{
    const auto partition = make_partition();
    MemoryWritableDevice device(4U * 1024U * 1024U);
    seed_valid_metadata(device, partition);

    device.bytes()[static_cast<std::size_t>(metadata_offset(partition))] = std::byte{0};

    ps2hdd::hdl::MetadataPatch patch;
    patch.compat_flags = 1;
    const auto result = ps2hdd::hdl::patch_game_metadata(device, partition, patch);
    check(!result.ok, "invalid HDL header must not be patched");
    check(device.write_calls == 0, "invalid HDL header must remain untouched");
}

} // namespace

int main()
{
    try {
        test_writable_file_backend_is_existing_and_fixed_size();
        test_patch_round_trips_selected_fields();
        test_patch_rejects_oversized_title_without_write();
        test_patch_rolls_back_failed_readback();
        test_patch_refuses_invalid_hdl_header();
        std::cout << "HDL write tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HDL write tests failed: " << error.what() << '\n';
        return 1;
    }
}
