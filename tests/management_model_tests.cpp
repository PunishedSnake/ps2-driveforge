#include "ps2hdd/management_model.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ps2hdd::PartitionCatalogEntry entry(const char* id, ps2hdd::PartitionCatalogKind kind,
                                    std::uint32_t lba, bool sub = false)
{
    ps2hdd::PartitionCatalogEntry e;
    e.id = id;
    e.kind = kind;
    e.start_lba = lba;
    e.is_sub = sub;
    return e;
}

void removal_preserves_surviving_enrichment()
{
    ps2hdd::PartitionCatalog catalog;
    catalog.entries.push_back(entry("__mbr", ps2hdd::PartitionCatalogKind::mbr, 0));
    catalog.entries.push_back(entry("PP.GAME-A", ps2hdd::PartitionCatalogKind::hdl, 100));
    catalog.entries.push_back(entry("PP.GAME-A", ps2hdd::PartitionCatalogKind::hdl, 110, true));
    catalog.entries.push_back(entry("PP.GAME-B", ps2hdd::PartitionCatalogKind::hdl, 200));
    catalog.entries.push_back(entry("+OPL", ps2hdd::PartitionCatalogKind::pfs, 300));

    ps2hdd::ManagementModel model(std::move(catalog));
    check(model.progress().total_hdl == 2 && model.progress().pending_hdl == 2,
          "two HDL mains begin pending");

    ps2hdd::hdl::GameResult game_b;
    game_b.ok = true;
    game_b.game.start_lba = 200;
    game_b.game.partition_id = "PP.GAME-B";
    game_b.game.title = "Survivor";
    check(model.apply_hdl_result(game_b).has_value(), "survivor enrichment applied");
    check(model.progress().ready_hdl == 1 && model.progress().pending_hdl == 1,
          "enrichment progress before removal");

    ps2hdd::apa::RemovePlan plan;
    plan.ok = true;
    plan.main_lba = 100;
    plan.partition_id = "PP.GAME-A";
    plan.removed_lbas = {100, 110};
    check(model.apply_partition_removal(plan), "removal plan updates model");

    check(model.rows().size() == 3, "main and sub rows removed in RAM");
    check(model.progress().total_hdl == 1 && model.progress().ready_hdl == 1 &&
              model.progress().pending_hdl == 0,
          "surviving enrichment progress preserved");

    bool survivor_found = false;
    bool opl_found = false;
    for (const auto& row : model.rows()) {
        if (row.partition.start_lba == 200) {
            survivor_found = row.hdl_state == ps2hdd::EnrichmentState::ready &&
                             row.hdl_game && row.hdl_game->title == "Survivor";
        }
        if (row.partition.start_lba == 300) {
            opl_found = true;
        }
    }
    check(survivor_found, "surviving HDL row keeps parsed metadata");
    check(opl_found, "unrelated PFS row survives removal");

    ps2hdd::hdl::GameResult stale;
    stale.ok = true;
    stale.game.start_lba = 100;
    check(!model.apply_hdl_result(stale).has_value(),
          "late background result for removed game is ignored");
}

} // namespace

int main()
{
    try {
        removal_preserves_surviving_enrichment();
        std::cout << "Management model removal tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Management model removal test failure: " << error.what() << '\n';
        return 1;
    }
}
