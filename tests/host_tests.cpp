#include "ps2hdd/management_model.hpp"
#include "ps2hdd/pfs_export.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ps2hdd::PartitionCatalogEntry catalog_entry(const char* id,
                                            ps2hdd::PartitionCatalogKind kind,
                                            std::uint32_t lba,
                                            bool sub = false)
{
    ps2hdd::PartitionCatalogEntry entry;
    entry.id = id;
    entry.kind = kind;
    entry.start_lba = lba;
    entry.is_sub = sub;
    return entry;
}

void management_removal_preserves_surviving_enrichment()
{
    ps2hdd::PartitionCatalog catalog;
    catalog.entries.push_back(catalog_entry("__mbr", ps2hdd::PartitionCatalogKind::mbr, 0));
    catalog.entries.push_back(catalog_entry("PP.GAME-A", ps2hdd::PartitionCatalogKind::hdl, 100));
    catalog.entries.push_back(catalog_entry("PP.GAME-A", ps2hdd::PartitionCatalogKind::hdl, 110, true));
    catalog.entries.push_back(catalog_entry("PP.GAME-B", ps2hdd::PartitionCatalogKind::hdl, 200));
    catalog.entries.push_back(catalog_entry("+OPL", ps2hdd::PartitionCatalogKind::pfs, 300));

    ps2hdd::ManagementModel model(std::move(catalog));
    check(model.progress().total_hdl == 2 && model.progress().pending_hdl == 2,
          "two HDL mains begin pending");

    ps2hdd::hdl::GameResult survivor;
    survivor.ok = true;
    survivor.game.start_lba = 200;
    survivor.game.partition_id = "PP.GAME-B";
    survivor.game.title = "Survivor";
    check(model.apply_hdl_result(survivor).has_value(), "survivor enrichment applied");

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
        using ps2hdd::pfs::sanitize_host_filename;
        check(sanitize_host_filename("CFG") == "CFG", "normal filename preserved");
        check(sanitize_host_filename("bad:name?.cfg") == "bad_name_.cfg", "illegal characters replaced");
        check(sanitize_host_filename("CON") == "_CON", "reserved device name escaped");
        check(sanitize_host_filename("LPT1.txt") == "_LPT1.txt", "reserved LPT device name escaped");
        check(sanitize_host_filename("trailing. ") == "trailing._", "trailing space escaped");
        check(sanitize_host_filename("ending.") == "ending_", "trailing dot escaped");
        check(sanitize_host_filename("..") == "_", "dot-dot cannot escape destination");
        management_removal_preserves_surviving_enrichment();
        std::cout << "All host/model tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        return 1;
    }
}
