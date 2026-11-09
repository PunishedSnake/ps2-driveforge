#pragma once

#include "ps2hdd/apa_repair.hpp"
#include "ps2hdd/block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2hdd::health {

// One deterministic, read-only answer to "is this PS2 HDD structurally OK and
// what is the narrowest fix?" It mirrors FHDB Manager's repair_health_assess
// (header defects + pointer bounds + payload KELF -> recommendation) and adds
// the host-only evidence DriveForge already has: the full APA chain parse, PC
// partition-map ownership and APA-Jail. Nothing here writes, and no action is
// recommended that the existing guarded writer would refuse.

enum class Verdict {
    healthy,     // nothing to do
    attention,   // readable, but something deserves a look (warnings only)
    repairable,  // a deterministic, guarded fix exists
    damaged,     // no deterministic fix; forensic evidence/manual work needed
    blocked,     // PC/GPT ownership or unreadable media: PS2 writes refused
};

enum class Action {
    capture_rescue,     // save a Rescue Capsule while the bootstrap is good
    repair_master,      // conservative canonical-field repair of sector 0-1
    disable_bootstrap,  // HDDMBR backup + clear osdStart/osdSize
    forensic_scan,      // chain is broken beyond the master: raw scan + maps
};

enum class PointerState {
    unknown,        // master not canonical, pointer not evaluated
    disabled,       // 0/0
    valid,          // in bounds, readable, structurally valid KELF
    half_empty,     // exactly one of start/size is zero
    out_of_bounds,  // outside __mbr, before 0x2000, > 4 MiB or past the device
    unreadable,     // in bounds but the payload could not be read
    not_kelf,       // payload read but fails structural KELF validation
};

enum class Severity { info, warning, error };

struct Finding {
    Severity severity{Severity::info};
    std::string text;
};

struct Report {
    Verdict verdict{Verdict::blocked};
    std::vector<Finding> findings;
    std::vector<Action> actions;  // ordered: do the first one first

    bool master_readable{};
    bool master_canonical{};
    repair::ApaRepairPlan master;
    bool pc_owned{};      // GPT / protective / foreign MBR
    bool apa_jail{};
    bool chain_ok{};
    std::size_t partitions{};
    std::size_t chain_errors{};
    std::size_t chain_warnings{};

    PointerState pointer{PointerState::unknown};
    std::uint32_t osd_start{};
    std::uint32_t osd_sectors{};
};

[[nodiscard]] Report assess(BlockDevice& device);

[[nodiscard]] const char* verdict_name(Verdict verdict) noexcept;
[[nodiscard]] const char* action_name(Action action) noexcept;
[[nodiscard]] const char* pointer_name(PointerState state) noexcept;
[[nodiscard]] bool recommends(const Report& report, Action action) noexcept;
[[nodiscard]] std::string render(const Report& report);

} // namespace ps2hdd::health
