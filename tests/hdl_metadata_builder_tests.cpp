#include "ps2hdd/hdl_metadata_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
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

std::uint32_t load_u32_le(const std::byte* p)
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

class SparseDevice final : public ps2hdd::BlockDevice {
public:
    explicit SparseDevice(std::uint64_t size) : size_(size) {}
    std::uint64_t size_bytes() const override { return size_; }
    std::string display_name() const override { return "metadata-sparse"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        const auto it = chunks_.find(offset);
        if (it == chunks_.end() || it->second.size() != out.size()) {
            return false;
        }
        std::memcpy(out.data(), it->second.data(), out.size());
        return true;
    }

    void put(std::uint64_t offset, std::span<const std::byte> bytes)
    {
        chunks_[offset] = std::vector<std::byte>(bytes.begin(), bytes.end());
    }

private:
    std::uint64_t size_{};
    std::map<std::uint64_t, std::vector<std::byte>> chunks_;
};

std::uint64_t mib(std::uint64_t value)
{
    return value * 1024ULL * 1024ULL;
}

ps2hdd::apa::AllocationPlan make_plan()
{
    ps2hdd::apa::AllocationPlan plan;
    plan.ok = true;
    plan.extents = {
        {2 * ps2hdd::apa::kAllocationChunkSectors,
         ps2hdd::apa::kAllocationChunkSectors, 0, 4 * ps2hdd::apa::kAllocationChunkSectors},
        {4 * ps2hdd::apa::kAllocationChunkSectors,
         ps2hdd::apa::kAllocationChunkSectors, 2 * ps2hdd::apa::kAllocationChunkSectors, 0},
    };
    plan.allocated_bytes = mib(256);
    plan.overhead_bytes = mib(5);
    plan.usable_payload_bytes = mib(251);
    return plan;
}

void test_metadata_round_trips_through_normal_reader()
{
    const auto plan = make_plan();
    ps2hdd::hdl::MetadataBuildRequest request;
    request.title = "Frieren Install Test";
    request.startup = "SLUS_123.45";
    request.compat_flags = 0x12;
    request.dma = 0x0040;
    request.media = ps2hdd::hdl::MediaType::dvd;
    request.payload_bytes = mib(130);

    const auto built = ps2hdd::hdl::build_install_metadata(plan, request);
    check(built.ok, "valid install metadata should build");
    check(built.payload_extents.size() == 2, "130 MiB should span main and first sub extent");
    check(built.payload_extents[0].bytes == mib(124), "main payload capacity should reserve 4 MiB");
    check(built.payload_extents[1].bytes == mib(6), "sub extent should receive remaining payload");
    check(built.payload_extents[0].source_offset_bytes == 0, "first source offset should be zero");
    check(built.payload_extents[1].source_offset_bytes == mib(124), "second source offset mismatch");

    const auto first_entry = ps2hdd::hdl::kAllocTableOffset;
    const auto second_entry = first_entry + ps2hdd::hdl::kAllocEntryBytes;
    check(load_u32_le(built.metadata.data() + first_entry) == 0, "first allocation offset units mismatch");
    check(load_u32_le(built.metadata.data() + first_entry + 8) == mib(124) / 256,
          "first allocation length units mismatch");
    check(load_u32_le(built.metadata.data() + second_entry) == mib(124) / (512ULL * 1024ULL),
          "second allocation source offset units mismatch");
    check(load_u32_le(built.metadata.data() + second_entry + 8) == mib(6) / 256,
          "second allocation length units mismatch");

    ps2hdd::apa::Partition partition;
    partition.id = "PP.SLUS-12345..FRIEREN_TEST";
    partition.start_lba = plan.extents[0].start_lba;
    partition.length_sectors = plan.extents[0].length_sectors;
    partition.total_sectors = static_cast<std::uint64_t>(plan.extents[0].length_sectors) +
                              plan.extents[1].length_sectors;
    partition.type = ps2hdd::apa::kTypeHdl;
    partition.sub_count = 1;
    partition.sub_partitions.push_back({plan.extents[1].start_lba, plan.extents[1].length_sectors});

    SparseDevice device(mib(1024));
    const auto metadata_offset =
        static_cast<std::uint64_t>(partition.start_lba) * ps2hdd::apa::kSectorSize +
        ps2hdd::hdl::kMetadataOffset;
    device.put(metadata_offset, built.metadata);

    const auto parsed = ps2hdd::hdl::read_game_info(device, partition);
    check(parsed.ok, "normal HDL reader should accept generated metadata");
    check(parsed.game.title == request.title, "generated title did not round-trip");
    check(parsed.game.startup == request.startup, "generated startup did not round-trip");
    check(parsed.game.compat_flags == request.compat_flags, "generated compat flags mismatch");
    check(parsed.game.dma == request.dma, "generated DMA mismatch");
    check(parsed.game.media == ps2hdd::hdl::MediaType::dvd, "generated media mismatch");
    check(parsed.game.declared_parts == 2, "generated part count mismatch");
    check(parsed.game.raw_size_bytes == request.payload_bytes, "generated raw payload size mismatch");
    check(parsed.game.allocation_table_consistent, "generated allocation table should match APA subs");
}

void test_cd_media_and_one_extent()
{
    auto plan = make_plan();
    plan.extents.resize(1);
    plan.allocated_bytes = mib(128);
    plan.overhead_bytes = mib(4);
    plan.usable_payload_bytes = mib(124);

    ps2hdd::hdl::MetadataBuildRequest request;
    request.title = "CD Test";
    request.startup = "SLES_543.21";
    request.media = ps2hdd::hdl::MediaType::cd;
    request.payload_bytes = mib(64);

    const auto built = ps2hdd::hdl::build_install_metadata(plan, request);
    check(built.ok, "single-extent CD metadata should build");
    check(built.payload_extents.size() == 1, "CD payload should use one extent");
    check(std::to_integer<unsigned char>(built.metadata[ps2hdd::hdl::kPartCountOffset]) == 1,
          "CD part count should be one");
}

void test_invalid_requests_are_refused()
{
    const auto plan = make_plan();
    ps2hdd::hdl::MetadataBuildRequest request;
    request.title = "Bad";
    request.startup = "GAME.ELF";
    request.payload_bytes = mib(1);
    check(!ps2hdd::hdl::build_install_metadata(plan, request).ok,
          "invalid startup must be refused");

    request.startup = "SLUS_123.45";
    request.payload_bytes = plan.usable_payload_bytes + 2048;
    check(!ps2hdd::hdl::build_install_metadata(plan, request).ok,
          "payload beyond allocation capacity must be refused");

    request.payload_bytes = 12345;
    check(!ps2hdd::hdl::build_install_metadata(plan, request).ok,
          "non-2048-aligned ISO payload must be refused");
}

} // namespace

int main()
{
    try {
        test_metadata_round_trips_through_normal_reader();
        test_cd_media_and_one_extent();
        test_invalid_requests_are_refused();
        std::cout << "HDL metadata builder tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HDL metadata builder tests failed: " << error.what() << '\n';
        return 1;
    }
}
