#include "ps2hdd/hdl.hpp"
#include "ps2hdd/partition_catalog.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void check(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

ps2hdd::apa::Partition make_partition(std::string id, std::uint16_t type,
                                      std::uint32_t start, std::uint32_t length,
                                      std::uint16_t flags = 0)
{
    ps2hdd::apa::Partition partition;
    partition.id = std::move(id);
    partition.type = type;
    partition.start_lba = start;
    partition.length_sectors = length;
    partition.total_sectors = length;
    partition.flags = flags;
    return partition;
}

void catalog_roundtrip()
{
    ps2hdd::apa::ScanResult scan;
    scan.mbr_valid = true;
    scan.apa_version = 2;

    auto mbr = make_partition("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x4000);
    auto pfs = make_partition("+OPL", ps2hdd::apa::kTypePfs, 0x4000, 0x8000);
    pfs.sub_count = 1;
    auto pfs_sub = make_partition("+OPL", ps2hdd::apa::kTypePfs, 0xC000, 0x4000,
                                  ps2hdd::apa::kFlagSub);
    pfs_sub.main_lba = pfs.start_lba;
    pfs_sub.number = 1;
    auto hdl = make_partition("PP.SLUS-00000.GAME", ps2hdd::apa::kTypeHdl, 0x10000, 0x10000);
    auto free = make_partition("__empty", ps2hdd::apa::kTypeFree, 0x20000, 0x2000);

    scan.partitions = {mbr, pfs, pfs_sub, hdl, free};

    const auto full = ps2hdd::build_partition_catalog(scan, true);
    check(full.entries.size() == 5, "full catalog preserves all APA rows");
    check(full.main_partitions == 4, "full catalog counts main partitions");
    check(full.sub_partitions == 1, "full catalog counts sub partitions");
    check(full.pfs_partitions == 1 && full.hdl_partitions == 1,
          "full catalog classifies PFS and HDL mains");
    check(full.pfs_bytes == pfs.size_bytes(), "PFS total counts only main volume size");
    check(full.hdl_bytes == hdl.size_bytes(), "HDL total is available without enrichment");
    check(full.free_bytes == free.size_bytes(), "free-space total is available without enrichment");
    check(full.entries[2].is_sub && full.entries[2].main_lba == pfs.start_lba,
          "sub-partition relationship survives catalog transformation");

    const auto mains = ps2hdd::build_partition_catalog(scan, false);
    check(mains.entries.size() == 4, "main-only catalog hides sub rows without rescanning");
    check(mains.sub_partitions == 1, "main-only catalog still reports hidden sub count");
    check(mains.entries[2].kind == ps2hdd::PartitionCatalogKind::hdl,
          "HDL rows are classified directly from APA type");
}

class MemoryDevice final : public ps2hdd::BlockDevice {
public:
    explicit MemoryDevice(std::size_t size) : bytes_(size) {}

    [[nodiscard]] std::uint64_t size_bytes() const override { return bytes_.size(); }
    [[nodiscard]] std::string display_name() const override { return "HDL fixture"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            return false;
        }
        ++read_calls;
        read_offsets.push_back(offset);
        std::memcpy(out.data(), bytes_.data() + static_cast<std::size_t>(offset), out.size());
        return true;
    }

    void write(std::uint64_t offset, std::span<const std::byte> data)
    {
        if (offset > bytes_.size() || data.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
            throw std::runtime_error("fixture write out of range");
        }
        std::memcpy(bytes_.data() + static_cast<std::size_t>(offset), data.data(), data.size());
    }

    std::uint64_t read_calls{};
    std::vector<std::uint64_t> read_offsets;

private:
    std::vector<std::byte> bytes_;
};

void store_u16(std::byte* p, std::uint16_t value)
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void store_u32(std::byte* p, std::uint32_t value)
{
    p[0] = static_cast<std::byte>(value & 0xFFU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

void write_hdl_header(MemoryDevice& device, const ps2hdd::apa::Partition& partition,
                      std::string_view title, std::string_view startup,
                      std::uint32_t first_length, std::uint32_t second_length)
{
    std::array<std::byte, ps2hdd::hdl::kMetadataBytes> header{};
    store_u32(header.data(), ps2hdd::hdl::kMagic);
    header[6] = std::byte{1};

    check(title.size() < ps2hdd::hdl::kTitleStorage, "fixture title fits HDL field");
    check(startup.size() < ps2hdd::hdl::kStartupStorage, "fixture startup fits HDL field");
    std::memcpy(header.data() + ps2hdd::hdl::kTitleOffset, title.data(), title.size());
    std::memcpy(header.data() + ps2hdd::hdl::kStartupOffset, startup.data(), startup.size());
    header[ps2hdd::hdl::kCompatOffset] = std::byte{0x21};
    store_u16(header.data() + ps2hdd::hdl::kDmaOffset, 0x1234);
    store_u32(header.data() + ps2hdd::hdl::kLayerBreakOffset, 0x11223344);
    store_u32(header.data() + ps2hdd::hdl::kMediaOffset, 0x14);
    header[ps2hdd::hdl::kPartCountOffset] = std::byte{2};
    store_u32(header.data() + ps2hdd::hdl::kAllocTableOffset + 8, first_length);
    store_u32(header.data() + ps2hdd::hdl::kAllocTableOffset + ps2hdd::hdl::kAllocEntryBytes + 8,
              second_length);

    const auto offset = static_cast<std::uint64_t>(partition.start_lba) * ps2hdd::apa::kSectorSize +
                        ps2hdd::hdl::kMetadataOffset;
    device.write(offset, header);
}

void native_hdl_metadata_contract()
{
    MemoryDevice device(32U * 1024U * 1024U);

    auto late = make_partition("PP.SLUS-22222.LATE", ps2hdd::apa::kTypeHdl, 0x5000, 0x5000);
    late.sub_count = 1;
    late.total_sectors += 0x1000;
    auto early = make_partition("PP.SLUS-11111.EARLY", ps2hdd::apa::kTypeHdl, 0x1000, 0x5000);
    early.sub_count = 1;
    early.total_sectors += 0x0800;

    write_hdl_header(device, late, "Late Game", "SLUS_222.22", 0x1000, 0x2000);
    write_hdl_header(device, early, "Early Game", "SLUS_111.11", 0x0800, 0x1800);

    const auto one = ps2hdd::hdl::read_game_info(device, late);
    check(one.ok, "native HDL metadata parser accepts valid 0xDEADFEED header");
    check(one.game.title == "Late Game" && one.game.startup == "SLUS_222.22",
          "native parser extracts title and startup without a helper process");
    check(one.game.compat_flags == 0x21 && one.game.dma == 0x1234,
          "native parser extracts compat flags and DMA mode");
    check(one.game.layer_break == 0x11223344 && one.game.media == ps2hdd::hdl::MediaType::dvd,
          "native parser extracts layer break and DVD media type");
    check(one.game.raw_size_bytes == static_cast<std::uint64_t>(0x3000) * 256ULL,
          "native parser follows hdl-dump allocation-table size units");
    check(one.game.allocated_size_bytes == late.size_bytes(),
          "allocated game size comes from already-scanned APA extents");
    check(one.game.allocation_table_consistent,
          "HDL part count agrees with APA main plus sub-partitions");
    check(device.read_calls == 1, "one game enrichment performs one metadata backing read");

    ps2hdd::apa::ScanResult scan;
    scan.mbr_valid = true;
    scan.apa_version = 2;
    // Deliberately reverse physical order. The native catalog must not inherit
    // arbitrary UI/APA ordering when a rotational disk can be serviced by LBA.
    scan.partitions = {late, early};

    device.read_calls = 0;
    device.read_offsets.clear();
    const auto catalog = ps2hdd::hdl::read_catalog(device, scan);
    check(catalog.entries.size() == 2 && catalog.readable == 2 && catalog.unreadable == 0,
          "native HDL catalog enriches all main game partitions");
    check(catalog.entries[0].game.partition_id == early.id &&
              catalog.entries[1].game.partition_id == late.id,
          "HDL enrichment is scheduled in ascending physical LBA order");
    check(device.read_calls == 2, "two games require exactly two in-process metadata reads");
    check(device.read_offsets[0] < device.read_offsets[1],
          "backing reads follow physical order rather than source list order");

    auto damaged = late;
    std::array<std::byte, 4> zero{};
    const auto damaged_offset = static_cast<std::uint64_t>(damaged.start_lba) * ps2hdd::apa::kSectorSize +
                                ps2hdd::hdl::kMetadataOffset;
    device.write(damaged_offset, zero);
    const auto invalid = ps2hdd::hdl::read_game_info(device, damaged);
    check(!invalid.ok, "missing HDL magic is rejected instead of exposing garbage metadata");
}

} // namespace

int main()
{
    try {
        catalog_roundtrip();
        native_hdl_metadata_contract();
        std::cout << "Emilia zero-I/O partition catalog and native HDL metadata tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
