#include "ps2hdd/hdl.hpp"
#include "ps2hdd/management_model.hpp"
#include "ps2hdd/partition_catalog.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <stop_token>
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

    ps2hdd::ManagementModel model(full);
    const auto initial = model.progress();
    check(model.rows().size() == 5, "management model first paint uses complete in-memory catalog");
    check(initial.total_hdl == 1 && initial.pending_hdl == 1 && initial.ready_hdl == 0,
          "HDL game rows start pending without blocking initial list construction");

    ps2hdd::hdl::GameResult enriched;
    enriched.ok = true;
    enriched.game.partition_id = hdl.id;
    enriched.game.start_lba = hdl.start_lba;
    enriched.game.title = "Instant Game";
    const auto updated = model.apply_hdl_result(enriched);
    check(updated.has_value() && model.rows()[*updated].hdl_state == ps2hdd::EnrichmentState::ready,
          "one progressive HDL result updates exactly one management row");
    check(model.rows()[*updated].hdl_game && model.rows()[*updated].hdl_game->title == "Instant Game",
          "management row exposes enriched game title without rebuilding catalog");
    const auto after = model.progress();
    check(after.pending_hdl == 0 && after.ready_hdl == 1 && after.failed_hdl == 0,
          "management progress tracks incremental enrichment state");
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
    std::size_t progress_calls = 0;
    const auto catalog = ps2hdd::hdl::read_catalog(
        device, scan,
        [&](std::size_t completed, std::size_t total, const ps2hdd::hdl::GameResult& game) {
            ++progress_calls;
            check(completed == progress_calls && total == 2 && game.ok,
                  "progress callback reports deterministic completed/total counts");
        });
    check(catalog.entries.size() == 2 && catalog.total_candidates == 2 &&
              catalog.readable == 2 && catalog.unreadable == 0 && !catalog.cancelled,
          "native HDL catalog enriches all main game partitions");
    check(progress_calls == 2, "progressive HDL loader reports each completed game once");
    check(catalog.entries[0].game.partition_id == early.id &&
              catalog.entries[1].game.partition_id == late.id,
          "HDL enrichment is scheduled in ascending physical LBA order");
    check(device.read_calls == 2, "two games require exactly two in-process metadata reads");
    check(device.read_offsets[0] < device.read_offsets[1],
          "backing reads follow physical order rather than source list order");

    std::stop_source stop;
    device.read_calls = 0;
    device.read_offsets.clear();
    const auto cancelled = ps2hdd::hdl::read_catalog(
        device, scan,
        [&](std::size_t completed, std::size_t, const ps2hdd::hdl::GameResult&) {
            if (completed == 1) {
                stop.request_stop();
            }
        },
        stop.get_token());
    check(cancelled.cancelled && cancelled.entries.size() == 1 && device.read_calls == 1,
          "HDL enrichment cancellation stops before reading the next game");

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
        std::cout << "Emilia zero-I/O catalog, management model and native HDL metadata tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
