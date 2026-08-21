#include "ps2hdd/partition_catalog.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

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

} // namespace

int main()
{
    try {
        catalog_roundtrip();
        std::cout << "Emilia zero-I/O partition catalog tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
