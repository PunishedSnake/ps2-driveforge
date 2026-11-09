#include "ps2hdd/structure_health.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/disk_layout_guard.hpp"
#include "ps2hdd/fhdb_rescue.hpp"
#include "ps2hdd/fhdb_rescue_capture.hpp"

#include <algorithm>
#include <array>
#include <sstream>

namespace ps2hdd::health {
namespace {

constexpr std::size_t kMbrLengthOffset = 0x044;
constexpr std::size_t kOsdStartOffset = 0x130;
constexpr std::size_t kOsdSizeOffset = 0x134;
constexpr std::uint32_t kProgramStartSector = 0x2000;      // FHDB reserved __mbr program area
constexpr std::uint32_t kMaxPayloadSectors = (4U << 20U) / 512U;  // FHDB 4 MiB policy
constexpr std::size_t kMaxChainFindings = 6;

[[nodiscard]] std::uint32_t load_u32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

void add(Report& report, Severity severity, std::string text)
{
    report.findings.push_back({severity, std::move(text)});
}

void recommend(Report& report, Action action)
{
    if (!recommends(report, action)) report.actions.push_back(action);
}

void describe_master(Report& report)
{
    using namespace repair;
    const auto issues = report.master.issues;
    const std::array<std::pair<std::uint32_t, const char*>, 8> names{{
        {issue_checksum, "APA master checksum mismatch"},
        {issue_apa_magic, "APA magic damaged"},
        {issue_mbr_id, "__mbr identifier damaged"},
        {issue_sony_magic, "Sony MBR signature damaged"},
        {issue_master_start, "master start is not sector 0"},
        {issue_master_type, "master APA type is not MBR"},
        {issue_mbr_version, "MBR version is not canonical"},
        {issue_pointer_inconsistent, "osdStart/osdSize are inconsistent"},
    }};
    for (const auto& [bit, text] : names) {
        // A canonical master gets its pointer judged by assess_pointer, which
        // says the same thing with more precision.
        if (bit == issue_pointer_inconsistent && report.master_canonical) continue;
        if ((issues & bit) != 0) add(report, Severity::error, text);
    }
    const auto blockers = report.master.blockers;
    if ((blockers & blocker_hybrid_gpt) != 0 && !report.apa_jail)
        add(report, Severity::error, "Repair blocker: PC MBR/GPT signature in sector 0");
    if ((blockers & blocker_hybrid_gpt) != 0 && report.apa_jail && issues != 0)
        add(report, Severity::warning, "APA-Jail master: automatic master field repair is refused (the PC table shares this sector); use forensic analysis");
    if ((blockers & blocker_low_identity) != 0)
        add(report, Severity::error, "Repair blocker: not enough APA identity evidence (" +
                                         std::to_string(report.master.identity_matches) + "/3)");
    if ((blockers & blocker_not_master) != 0)
        add(report, Severity::error, "Repair blocker: sector 0 is not reliably the APA master");
    if ((blockers & blocker_unexplained_checksum) != 0)
        add(report, Severity::error, "Repair blocker: checksum does not corroborate one exact repair");
}

void assess_pointer(Report& report, BlockDevice& device,
                    const std::array<std::byte, repair::kApaHeaderBytes>& header)
{
    report.osd_start = load_u32(header.data() + kOsdStartOffset);
    report.osd_sectors = load_u32(header.data() + kOsdSizeOffset);
    const auto mbr_length = load_u32(header.data() + kMbrLengthOffset);
    const auto start = report.osd_start;
    const auto sectors = report.osd_sectors;

    if (start == 0 && sectors == 0) {
        report.pointer = PointerState::disabled;
        add(report, Severity::info, "HDD bootstrap pointer is disabled (console boots from memory card / disc)");
        return;
    }
    if (start == 0 || sectors == 0) {
        report.pointer = PointerState::half_empty;
        add(report, Severity::error, "Bootstrap pointer is half-empty (one of osdStart/osdSize is zero)");
        return;
    }
    const auto device_sectors = device.size_bytes() / 512U;
    if (start < kProgramStartSector || sectors > kMaxPayloadSectors || start >= mbr_length ||
        sectors > mbr_length - start ||
        static_cast<std::uint64_t>(start) + sectors > device_sectors) {
        report.pointer = PointerState::out_of_bounds;
        add(report, Severity::error, "Bootstrap pointer is outside the reserved __mbr program area");
        return;
    }
    // capture_rescue_image re-reads exactly the published range and earns
    // VALID_KELF from the shared structural validator.
    const auto capture = fhdb::capture_rescue_image(device);
    if (!capture.ok) {
        report.pointer = PointerState::unreadable;
        add(report, Severity::error, "Bootstrap payload could not be read: " + capture.error);
        return;
    }
    if ((capture.info.flags & fhdb::kRescueFlagValidKelf) == 0) {
        report.pointer = PointerState::not_kelf;
        add(report, Severity::error, "Bootstrap payload is not a structurally valid KELF");
        return;
    }
    report.pointer = PointerState::valid;
    add(report, Severity::info, "Bootstrap payload: valid KELF, " + std::to_string(capture.info.kelf_file_bytes) +
                                    " bytes at sector " + std::to_string(start));
}

} // namespace

Report assess(BlockDevice& device)
{
    Report report;
    std::array<std::byte, repair::kApaHeaderBytes> header{};
    if (device.size_bytes() < header.size() || !device.read(0, header)) {
        add(report, Severity::error, "Sectors 0-1 (APA master) could not be read");
        report.verdict = Verdict::blocked;
        return report;
    }
    report.master_readable = true;

    report.apa_jail = fhdb::is_apa_jail_master(header);
    const auto layout = inspect_disk_layout(device);
    report.pc_owned = !layout.ok || !layout.allows_ps2_mutation() || fhdb::is_pc_owned_master(header);
    if (report.pc_owned) {
        add(report, Severity::error, std::string("PC partition-map ownership detected (") +
                                         (layout.ok ? legacy_partition_map_name(layout.kind) : "unreadable") +
                                         "); DriveForge refuses PS2 writes to this disk");
        report.verdict = Verdict::blocked;
        return report;
    }
    if (report.apa_jail) add(report, Severity::info, "APA-Jail hybrid layout (APA + exFAT/OPL)");

    report.master = repair::analyze_apa_master(header);
    report.master_canonical = fhdb::is_standard_apa_master(header);
    describe_master(report);

    apa::Reader reader(device);
    const auto scan = reader.scan();
    report.chain_ok = scan.ok();
    report.partitions = scan.partitions.size();
    for (const auto& issue : scan.issues) {
        const bool error = issue.severity == apa::IssueSeverity::error;
        (error ? report.chain_errors : report.chain_warnings) += 1;
        if (report.chain_errors + report.chain_warnings <= kMaxChainFindings)
            add(report, error ? Severity::error : Severity::warning,
                "APA chain @" + std::to_string(issue.lba) + ": " + issue.message);
    }

    if (!report.master_canonical) {
        if (report.master.header_patch_safe) recommend(report, Action::repair_master);
        else recommend(report, Action::forensic_scan);
    } else {
        assess_pointer(report, device, header);
        const bool bad_pointer = report.pointer == PointerState::half_empty ||
                                 report.pointer == PointerState::out_of_bounds ||
                                 report.pointer == PointerState::unreadable ||
                                 report.pointer == PointerState::not_kelf;
        if (bad_pointer || report.master.pointer_clear_recommended)
            recommend(report, Action::disable_bootstrap);
        if (!report.chain_ok) {
            add(report, Severity::error, "The APA chain beyond the master is broken; normal writes are refused");
            recommend(report, Action::forensic_scan);
        }
    }
    if (report.master_canonical && report.chain_ok && report.pointer == PointerState::valid)
        recommend(report, Action::capture_rescue);

    if (recommends(report, Action::forensic_scan) && !recommends(report, Action::repair_master)) {
        report.verdict = Verdict::damaged;
    } else if (recommends(report, Action::repair_master) || recommends(report, Action::disable_bootstrap)) {
        report.verdict = Verdict::repairable;
    } else if (report.chain_warnings > 0) {
        report.verdict = Verdict::attention;
    } else {
        report.verdict = Verdict::healthy;
    }
    if (report.verdict == Verdict::healthy)
        add(report, Severity::info, "APA master, chain (" + std::to_string(report.partitions) +
                                        " partitions) and bootstrap pointer are consistent");
    return report;
}

const char* verdict_name(Verdict verdict) noexcept
{
    switch (verdict) {
    case Verdict::healthy: return "healthy";
    case Verdict::attention: return "attention";
    case Verdict::repairable: return "repairable";
    case Verdict::damaged: return "damaged";
    case Verdict::blocked: return "blocked";
    }
    return "unknown";
}

const char* action_name(Action action) noexcept
{
    switch (action) {
    case Action::capture_rescue: return "Create a Rescue Capsule (backup while the bootstrap is good)";
    case Action::repair_master: return "Repair APA master (canonical fields + checksum, sectors 0-1 only)";
    case Action::disable_bootstrap: return "Disable bootstrap (HDDMBR backup, then clear osdStart/osdSize)";
    case Action::forensic_scan: return "Forensic APA analysis (raw scan, candidate maps, FORENSIC.TXT)";
    }
    return "unknown";
}

const char* pointer_name(PointerState state) noexcept
{
    switch (state) {
    case PointerState::unknown: return "not evaluated";
    case PointerState::disabled: return "disabled";
    case PointerState::valid: return "valid KELF";
    case PointerState::half_empty: return "half-empty";
    case PointerState::out_of_bounds: return "out of bounds";
    case PointerState::unreadable: return "unreadable payload";
    case PointerState::not_kelf: return "not a KELF";
    }
    return "unknown";
}

bool recommends(const Report& report, Action action) noexcept
{
    return std::find(report.actions.begin(), report.actions.end(), action) != report.actions.end();
}

std::string render(const Report& report)
{
    std::ostringstream out;
    out << "Verdict: " << verdict_name(report.verdict) << '\n'
        << "APA master: " << (report.master_canonical ? "canonical" : "damaged")
        << " (identity " << report.master.identity_matches << "/3, anchors "
        << report.master.master_anchor_matches << "/3)\n"
        << "APA chain: " << (report.chain_ok ? "ok" : "broken") << ", " << report.partitions
        << " partition(s), " << report.chain_errors << " error(s), " << report.chain_warnings
        << " warning(s)\n"
        << "Bootstrap pointer: " << pointer_name(report.pointer);
    if (report.pointer != PointerState::unknown && report.pointer != PointerState::disabled)
        out << " (osdStart=0x" << std::hex << report.osd_start << ", osdSize=0x" << report.osd_sectors << std::dec << ')';
    out << '\n';
    for (const auto& finding : report.findings) {
        out << (finding.severity == Severity::error ? "  [ERROR] " :
                finding.severity == Severity::warning ? "  [WARN]  " : "  [INFO]  ")
            << finding.text << '\n';
    }
    if (report.actions.empty()) {
        out << "Recommended: nothing\n";
    } else {
        out << "Recommended, in order:\n";
        for (std::size_t i = 0; i < report.actions.size(); ++i)
            out << "  " << (i + 1) << ". " << action_name(report.actions[i]) << '\n';
    }
    return out.str();
}

} // namespace ps2hdd::health
