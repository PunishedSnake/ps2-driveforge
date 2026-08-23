#include "ps2hdd/management_model.hpp"
#include "ps2hdd/partition_catalog.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

void grouped_hdl_game_owns_named_and_unnamed_subparts()
{
    ps2hdd::apa::ScanResult scan;
    scan.mbr_valid = true;
    scan.apa_version = 2;

    auto mbr = make_partition("__mbr", ps2hdd::apa::kTypeMbr, 0, 0x4000);
    auto game = make_partition("PP.SLUS-12345..EXAMPLE", ps2hdd::apa::kTypeHdl,
                               0x40000, 0x20000);
    game.sub_count = 2;
    game.total_sectors += 0x10000 + 0x08000;

    // Deliberately place the children out of logical number order and give one
    // an arbitrary name. Ownership must come from main_lba, not adjacency/name.
    auto child_two = make_partition("mystery-name", ps2hdd::apa::kTypeHdl,
                                    0x70000, 0x08000, ps2hdd::apa::kFlagSub);
    child_two.main_lba = game.start_lba;
    child_two.number = 2;

    auto child_one = make_partition("", ps2hdd::apa::kTypeHdl,
                                    0x60000, 0x10000, ps2hdd::apa::kFlagSub);
    child_one.main_lba = game.start_lba;
    child_one.number = 1;

    auto orphan = make_partition("orphan", ps2hdd::apa::kTypeHdl,
                                 0x78000, 0x04000, ps2hdd::apa::kFlagSub);
    orphan.main_lba = 0xDEADBEEFu;
    orphan.number = 1;

    scan.partitions = {mbr, game, child_two, child_one, orphan};

    const auto catalog = ps2hdd::build_partition_catalog(scan, true);
    const auto grouping = ps2hdd::group_partition_catalog(catalog);

    check(grouping.groups.size() == 2, "MBR and HDL main should form two top-level groups");
    check(grouping.orphan_sub_partitions.size() == 1,
          "unresolvable subpartition must remain an explicit orphan");

    const ps2hdd::PartitionCatalogGroup* hdl_group = nullptr;
    for (const auto& group : grouping.groups) {
        if (group.is_hdl_game()) {
            hdl_group = &group;
            break;
        }
    }
    check(hdl_group != nullptr, "HDL game group missing");
    check(hdl_group->sub_partitions.size() == 2,
          "both named and unnamed HDL children must be folded under their owner");
    check(hdl_group->sub_partitions[0].number == 1 &&
              hdl_group->sub_partitions[1].number == 2,
          "children must be presented in APA subpartition-number order");
    check(hdl_group->sub_partitions[0].id.empty() &&
              hdl_group->sub_partitions[1].id == "mystery-name",
          "grouping must not require a child naming convention");

    const auto expected_main_extent =
        static_cast<std::uint64_t>(game.length_sectors) * ps2hdd::apa::kSectorSize;
    const auto expected_child_bytes =
        static_cast<std::uint64_t>(child_one.length_sectors + child_two.length_sectors) *
        ps2hdd::apa::kSectorSize;
    check(hdl_group->logical_size_bytes == game.size_bytes(),
          "group logical size must use already-aggregated APA main size");
    check(hdl_group->subpartition_bytes == expected_child_bytes,
          "group child-byte summary mismatch");
    check(hdl_group->physical_extent_bytes == expected_main_extent + expected_child_bytes,
          "group reconstructed physical extent total mismatch");
    check(hdl_group->physical_extent_bytes == hdl_group->logical_size_bytes,
          "healthy grouped HDL allocation should agree without double-counting children");
    check(hdl_group->complete, "all declared HDL subpartitions should make group complete");
}

void management_groups_survive_enrichment_and_delete_as_one_game()
{
    ps2hdd::apa::ScanResult scan;
    scan.mbr_valid = true;
    scan.apa_version = 2;

    auto game = make_partition("PP.SLES-50000..GROUPED", ps2hdd::apa::kTypeHdl,
                               0x10000, 0x10000);
    game.sub_count = 2;
    game.total_sectors += 0x08000 + 0x04000;

    auto sub1 = make_partition("", ps2hdd::apa::kTypeHdl,
                               0x20000, 0x08000, ps2hdd::apa::kFlagSub);
    sub1.main_lba = game.start_lba;
    sub1.number = 1;
    auto sub2 = make_partition("", ps2hdd::apa::kTypeHdl,
                               0x28000, 0x04000, ps2hdd::apa::kFlagSub);
    sub2.main_lba = game.start_lba;
    sub2.number = 2;

    scan.partitions = {game, sub1, sub2};
    ps2hdd::ManagementModel model(ps2hdd::build_partition_catalog(scan, true));

    check(model.groups().size() == 1 && model.groups()[0].sub_rows.size() == 2,
          "management model should expose one collapsible HDL game group");
    const auto main_row = model.groups()[0].main_row;
    check(model.rows()[main_row].partition.start_lba == game.start_lba,
          "management group main row mismatch");

    ps2hdd::hdl::GameResult enrichment;
    enrichment.ok = true;
    enrichment.game.start_lba = game.start_lba;
    enrichment.game.partition_id = game.id;
    enrichment.game.title = "Grouped Example";
    const auto updated = model.apply_hdl_result(enrichment);
    check(updated && *updated == main_row,
          "HDL enrichment must update the grouped main row without changing group identity");
    check(model.groups().size() == 1 &&
              model.rows()[model.groups()[0].main_row].hdl_game &&
              model.rows()[model.groups()[0].main_row].hdl_game->title == "Grouped Example",
          "enriched game title must remain available through grouped view");

    ps2hdd::apa::RemovePlan removal;
    removal.ok = true;
    removal.main_lba = game.start_lba;
    removal.partition_id = game.id;
    removal.removed_lbas = {game.start_lba, sub1.start_lba, sub2.start_lba};
    check(model.apply_partition_removal(removal),
          "committed game removal should update grouped management model");
    check(model.rows().empty() && model.groups().empty(),
          "removing one HDL game should remove its main and all collapsed children together");
}

} // namespace

int main()
{
    try {
        grouped_hdl_game_owns_named_and_unnamed_subparts();
        management_groups_survive_enrichment_and_delete_as_one_game();
        std::cout << "Partition grouping UX tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Partition grouping UX test failure: " << error.what() << '\n';
        return 1;
    }
}
