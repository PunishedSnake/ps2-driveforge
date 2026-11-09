#include "apa_fixture.hpp"

#include "ps2hdd/boot_chain.hpp"
#include "ps2hdd/file_block_device.hpp"

#include <iostream>

namespace {

using fixture::require;
namespace bc = ps2hdd::bootchain;

std::filesystem::path dir(const char* name)
{
    const auto path = std::filesystem::temp_directory_path() / (std::string("ps2df-bootchain-") + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

bc::Report inspect(const std::filesystem::path& image, const bc::Options& options = {})
{
    ps2hdd::FileBlockDevice disk(image);
    require(disk.is_open(), "open image");
    return bc::inspect(disk, options);
}

void config_parsing()
{
    const std::string text = "# comment\n  BOOT_AUTO = $PSBBN  # trailing\r\nboot_auto_arg1 = -dev9=NICHDD\nboot_cross=$HOSDSYS\n";
    require(bc::config_value(text, "boot_auto").value_or("") == "$PSBBN", "case-insensitive key, trimmed value, comment stripped");
    require(bc::config_value(text, "boot_auto_arg1").value_or("") == "-dev9=NICHDD", "arg key is distinct from its prefix");
    require(!bc::config_value("#boot_auto = x\n", "boot_auto"), "commented line ignored");
    std::string value;
    require(bc::parse_boot_auto(text, value) == bc::AutoTarget::psbbn, "boot_auto $PSBBN");
    require(bc::parse_boot_auto("boot_auto = mc?:/BOOT/BOOT.ELF", value) == bc::AutoTarget::custom &&
            value == "mc?:/BOOT/BOOT.ELF", "custom target keeps its path");
    require(bc::parse_skip_hdd("OSDSYS_Skip_HDD = 1") == true && bc::parse_skip_hdd("Skip_HDD = off") == false &&
            !bc::parse_skip_hdd("Other = 1"), "Skip_HDD spellings");
    require(bc::find_fhdb_auto_target("LK_Auto_E2 = hdd0:x\nLK_Auto_E3 = y").value_or("") == "hdd0:x", "E1 -> E2 -> E3 order");
}

void psbbn_definitive_layout()
{
    const auto root = dir("psbbn");
    const auto image = root / "disk.img";
    fixture::build(image, {{"__sysconf", "__system", "__linux.1", "__linux.4", "__linux.5", "__contents"}, 10});
    fixture::put_files(image, "__sysconf", {{"/osdmenu/OSDMBR.CNF",
        "boot_auto = $PSBBN\nboot_auto_arg1 = -dev9=NICHDD\nboot_cross = $HOSDSYS\n"
        "boot_circle = $PSBBN\nboot_circle_arg1 = --kernel\nboot_circle_arg2 = pfs0:/p2lboot/ps2-linux-ntsc\n"
        "boot_circle_arg3 = -noflags\nprefer_bbn = 1\n"}});
    fixture::put_files(image, "__system", {{"/p2lboot/osdboot.elf", "elf"}, {"/p2lboot/vmlinux", "kernel"},
                                           {"/osdmenu/hosdmenu.elf", "hosd"}});
    const auto report = inspect(image);
    if (!report.ok) std::cerr << report.error << '\n';
    require(report.ok, "report ok");
    std::cout << bc::render(report);
    require(report.family == "PSBBN / OSDMenu MBR" && report.confidence == "high", "PSBBN family with high confidence");
    require(report.next_stage == "hdd0:__system:pfs:/p2lboot/osdboot.elf", "next stage is osdboot");
    const auto& e = report.evidence;
    require(e.psbbn_kernel && e.p2lboot_files.size() == 2 && e.psbbn_partition_count == 4, "PSBBN evidence");
    require(e.osdmenu_prefer_bbn && e.osdmenu_buttons.size() == 3, "button map parsed");
    const auto& circle = e.osdmenu_buttons[2];
    require(circle.key == "boot_circle" && circle.args.size() == 3 && circle.args[1] == "pfs0:/p2lboot/ps2-linux-ntsc",
            "boot_circle args in order");

    // Memory-card folder with FMCB Skip_HDD on.
    std::filesystem::create_directories(root / "mc0" / "SYS-CONF");
    std::ofstream(root / "mc0" / "SYS-CONF" / "FREEMCB.CNF") << "OSDSYS_Skip_HDD = 1\n";
    bc::Options options;
    options.memory_card_root = root / "mc0";
    const auto with_mc = inspect(image, options);
    require(with_mc.evidence.fmcb_skip_hdd == true, "Skip_HDD read from the memory-card folder");
    bool noted = false;
    for (const auto& note : with_mc.notes) noted |= note.find("Skip_HDD") != std::string::npos;
    require(noted, "Skip_HDD consequence explained");
}

void osdmenu_outranks_stale_psbbn()
{
    const auto image = dir("hosd") / "disk.img";
    fixture::build(image, {{"__sysconf", "__system", "__linux.1", "__linux.4", "__linux.5", "__linux.6"}, 10});
    fixture::put_files(image, "__sysconf", {{"/osdmenu/OSDMBR.CNF", "boot_auto = $HOSDSYS\n"}});
    fixture::put_files(image, "__system", {{"/osdmenu/hosdmenu.elf", "hosd"}});
    const auto report = inspect(image);
    require(report.family == "HDD-OSD / OSDMenu MBR" &&
            report.next_stage == "hdd0:__system:pfs:/osdmenu/hosdmenu.elf", "explicit OSDMenu config wins (Torii rule)");
    bool stale = false;
    for (const auto& note : report.notes) stale |= note.find("PSBBN partitions exist") != std::string::npos;
    require(stale, "stale PSBBN partitions are pointed out");
}

void fhdb_and_disabled()
{
    const auto image = dir("fhdb") / "disk.img";
    fixture::build(image, {{"__sysconf", "__system"}, 4});
    fixture::put_files(image, "__sysconf", {{"/FMCB/FREEHDB.CNF", "LK_Auto_E1 = hdd0:__system:pfs:/osd/osdmain.elf\n"}});
    fixture::put_files(image, "__system", {{"/osd/osdmain.elf", "osd"}});
    const auto report = inspect(image);
    require(report.family == "FHDB-compatible MBR" && report.next_stage == "hdd0:__system:pfs:/osd/osdmain.elf" &&
            report.evidence.fhdb_auto_path == "hdd0:__system:pfs:/osd/osdmain.elf", "FHDB family + LK_Auto");

    const auto off = dir("off") / "disk.img";
    fixture::Spec spec{{"__sysconf"}, 4};
    spec.kelf = false;
    fixture::build(off, spec);
    const auto none = inspect(off);
    require(none.family == "No active payload" && none.confidence == "certain", "no pointer -> no payload");
}

} // namespace

int main()
{
    try {
        config_parsing();
        psbbn_definitive_layout();
        osdmenu_outranks_stale_psbbn();
        fhdb_and_disabled();
    } catch (const std::exception& error) {
        std::cerr << "boot chain test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Boot chain tests passed\n";
    return 0;
}
