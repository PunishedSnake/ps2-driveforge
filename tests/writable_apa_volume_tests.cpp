#include "ps2hdd/writable_apa_volume.hpp"

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

class MemoryDevice final : public ps2hdd::WritableBlockDevice {
public:
    explicit MemoryDevice(std::size_t bytes) : data(bytes) {}

    std::uint64_t size_bytes() const override { return data.size(); }
    std::string display_name() const override { return "writable-apa-fixture"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > data.size() || out.size() > data.size() - offset) {
            return false;
        }
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        ++writes;
        if (offset > data.size() || in.size() > data.size() - offset) {
            return false;
        }
        std::copy(in.begin(), in.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
        last_offset = offset;
        return true;
    }

    bool flush() override
    {
        ++flushes;
        return true;
    }

    std::vector<std::byte> data;
    std::size_t writes{};
    std::size_t flushes{};
    std::uint64_t last_offset{};
};

ps2hdd::apa::Partition fixture_partition()
{
    ps2hdd::apa::Partition partition;
    partition.id = "+OPL";
    partition.type = ps2hdd::apa::kTypePfs;
    partition.start_lba = 0x100;
    partition.length_sectors = 0x20;
    partition.sub_partitions.push_back({0x200, 0x10});
    partition.total_sectors = 0x30;
    return partition;
}

void test_main_and_sub_mapping()
{
    MemoryDevice disk(0x300ULL * ps2hdd::apa::kSectorSize);
    ps2hdd::WritableApaVolume volume(disk, fixture_partition());
    check(volume.extent_count() == 2, "main/sub extent count mismatch");

    std::vector<std::byte> sector(ps2hdd::apa::kSectorSize, std::byte{0x5A});
    check(volume.write_sectors(0, 3, 1, sector), "main extent write failed");
    check(disk.last_offset == (0x100ULL + 3ULL) * ps2hdd::apa::kSectorSize,
          "main extent mapped to wrong device offset");

    std::fill(sector.begin(), sector.end(), std::byte{0xA5});
    check(volume.write_sectors(1, 4, 1, sector), "sub extent write failed");
    check(disk.last_offset == (0x200ULL + 4ULL) * ps2hdd::apa::kSectorSize,
          "sub extent mapped to wrong device offset");
}

void test_bounds_refused_before_backend_write()
{
    MemoryDevice disk(0x300ULL * ps2hdd::apa::kSectorSize);
    ps2hdd::WritableApaVolume volume(disk, fixture_partition());
    std::vector<std::byte> sector(ps2hdd::apa::kSectorSize, std::byte{0x11});

    check(!volume.write_sectors(0, 0x20, 1, sector), "write past main extent was accepted");
    check(!volume.write_sectors(1, 0x10, 1, sector), "write past sub extent was accepted");
    check(!volume.write_sectors(2, 0, 1, sector), "unknown sub index was accepted");
    check(disk.writes == 0, "invalid range reached backing device write");
}

void test_exact_byte_count_required()
{
    MemoryDevice disk(0x300ULL * ps2hdd::apa::kSectorSize);
    ps2hdd::WritableApaVolume volume(disk, fixture_partition());
    std::vector<std::byte> short_buffer(ps2hdd::apa::kSectorSize - 1, std::byte{0x22});
    check(!volume.write_sectors(0, 0, 1, short_buffer), "short sector buffer was accepted");
    check(disk.writes == 0, "short sector buffer reached backing write");
}

void test_readback_and_flush_forwarding()
{
    MemoryDevice disk(0x300ULL * ps2hdd::apa::kSectorSize);
    ps2hdd::WritableApaVolume volume(disk, fixture_partition());
    std::vector<std::byte> written(ps2hdd::apa::kSectorSize, std::byte{0x7C});
    std::vector<std::byte> readback(ps2hdd::apa::kSectorSize);
    check(volume.write_sectors(0, 1, 1, written), "fixture write failed");
    check(volume.read_sectors(0, 1, 1, readback), "volume readback failed");
    check(readback == written, "volume readback mismatch");
    check(volume.flush(), "volume flush failed");
    check(disk.flushes == 1, "volume flush was not forwarded exactly once");
}

} // namespace

int main()
{
    try {
        test_main_and_sub_mapping();
        test_bounds_refused_before_backend_write();
        test_exact_byte_count_required();
        test_readback_and_flush_forwarding();
        std::cout << "Writable APA volume tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Writable APA volume test failure: " << error.what() << '\n';
        return 1;
    }
}
