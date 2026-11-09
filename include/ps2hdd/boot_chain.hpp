#pragma once

#include "ps2hdd/block_device.hpp"
#include "ps2hdd/structure_health.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd::bootchain {

// "What will this PS2 HDD boot into, and why?" - read-only.
//
// Host port of FHDB Manager's boot-chain evidence + classify_boot_chain(): the
// same evidence keys, the same rule order (explicit OSDMenu configuration
// outranks stale partitions from an older install) and the same family labels,
// so a report from the console and one from the PC agree. DriveForge adds what
// a PC can see that the console screen does not print: the full OSDMBR.CNF
// button map, the PSBBN kernel files and, optionally, a memory-card folder dump
// for FMCB's Skip_HDD setting.

enum class AutoTarget { none, psbbn, hosdsys, custom };

struct Button {
    std::string key;                // boot_auto, boot_cross, ...
    std::string target;             // $PSBBN, $HOSDSYS, path or empty
    std::vector<std::string> args;  // boot_<key>_arg1..N
};

struct Evidence {
    // Bootstrap pointer (MBR program) from the structure-health pass.
    health::PointerState pointer{health::PointerState::unknown};
    std::uint32_t osd_start{};
    std::uint32_t osd_sectors{};

    // __sysconf
    bool sysconf_present{};
    bool fhdb_config{};
    std::string fhdb_config_path;
    std::string fhdb_auto_path;  // LK_Auto_E1 -> E2 -> E3
    bool fhdb_boot_elf{};
    bool osdmenu_mbr_config{};
    AutoTarget osdmenu_auto{AutoTarget::none};
    std::string osdmenu_auto_path;
    std::vector<Button> osdmenu_buttons;
    bool osdmenu_prefer_bbn{};

    // __system
    bool system_present{};
    bool psbbn_boot{};          // p2lboot/osdboot.elf
    bool psbbn_kernel{};        // p2lboot/vmlinux
    std::vector<std::string> p2lboot_files;
    bool hosdmenu_boot{};       // osdmenu/hosdmenu.elf
    bool hddosd_boot{};         // osd100/hosdsys.elf or OSDSYS_A.XLF
    std::string hddosd_path;
    bool legacy_osdmain{};      // osd/osdmain.elf
    std::size_t psbbn_partition_count{};

    // Optional memory-card folder: SYS-CONF/FREEMCB.CNF Skip_HDD.
    std::optional<bool> fmcb_skip_hdd;
    std::string fmcb_config_path;
};

struct Report {
    bool ok{};
    std::string error;
    Evidence evidence;
    std::string family;      // FHDB vocabulary: "PSBBN / OSDMenu MBR", ...
    std::string confidence;  // certain / high / medium / low
    std::string next_stage;  // hdd0:__system:pfs:/... or a description
    std::vector<std::string> notes;
};

struct Options {
    // Folder holding a memory card's contents (the one containing SYS-CONF).
    std::filesystem::path memory_card_root;
};

[[nodiscard]] Report inspect(BlockDevice& device, const Options& options = {});
[[nodiscard]] std::string render(const Report& report);

// Portable config helpers, identical semantics to FHDB boot_chain.c.
[[nodiscard]] std::optional<std::string> config_value(std::string_view text, std::string_view key);
[[nodiscard]] std::optional<bool> parse_skip_hdd(std::string_view text);
[[nodiscard]] AutoTarget parse_boot_auto(std::string_view text, std::string& value);
[[nodiscard]] std::optional<std::string> find_fhdb_auto_target(std::string_view text);
void classify(Report& report);

} // namespace ps2hdd::bootchain
