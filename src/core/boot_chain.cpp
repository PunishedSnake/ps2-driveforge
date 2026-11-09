#include "ps2hdd/boot_chain.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <sstream>

namespace ps2hdd::bootchain {
namespace {

constexpr std::uint64_t kTextLimit = 64U * 1024U;  // FHDB TEXT_FILE_LIMIT class

bool equal_nocase(char a, char b)
{
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
}

// Read-only view of one PFS partition by APA id.
class PfsView {
public:
    PfsView(BlockDevice& device, const apa::ScanResult& scan, std::string_view id)
    {
        const auto it = std::find_if(scan.partitions.begin(), scan.partitions.end(), [id](const apa::Partition& p) {
            return !p.is_sub() && p.id == id;
        });
        if (it == scan.partitions.end() || it->type != apa::kTypePfs) return;
        volume_ = std::make_unique<ApaVolume>(device, *it);
        reader_ = std::make_unique<pfs::Reader>(*volume_);
        if (!reader_->valid()) reader_.reset();
    }

    [[nodiscard]] bool valid() const noexcept { return reader_ != nullptr; }

    [[nodiscard]] bool exists(std::string_view path) const
    {
        return valid() && reader_->resolve(path).has_value();
    }

    [[nodiscard]] std::optional<std::string> read_text(std::string_view path) const
    {
        if (!valid()) return std::nullopt;
        const auto node = reader_->resolve(path);
        if (!node || (node->inode.mode & pfs::kModeMask) != pfs::kModeRegular) return std::nullopt;
        const auto size = std::min<std::uint64_t>(node->inode.size, kTextLimit);
        std::string text(static_cast<std::size_t>(size), '\0');
        if (size != 0 && !reader_->read(*node, 0, std::as_writable_bytes(std::span(text)))) return std::nullopt;
        return text;
    }

    [[nodiscard]] std::vector<std::string> list(std::string_view path) const
    {
        std::vector<std::string> names;
        if (!valid()) return names;
        const auto node = reader_->resolve(path);
        if (!node) return names;
        for (const auto& entry : reader_->list_directory(*node)) names.push_back(entry.name);
        std::sort(names.begin(), names.end());
        return names;
    }

private:
    std::unique_ptr<ApaVolume> volume_;
    std::unique_ptr<pfs::Reader> reader_;
};

std::optional<std::string> read_host_text(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string text(static_cast<std::size_t>(kTextLimit), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

std::optional<bool> parse_boolean(std::string_view value)
{
    if (value.empty()) return std::nullopt;
    const char first = static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
    const char second = value.size() > 1 ? static_cast<char>(std::tolower(static_cast<unsigned char>(value[1]))) : '\0';
    if (first == '1' || first == 'y' || first == 't' || (first == 'o' && second == 'n')) return true;
    if (first == '0' || first == 'n' || first == 'f' || (first == 'o' && second == 'f')) return false;
    return std::nullopt;
}

void scan_osdmenu_buttons(Evidence& evidence, std::string_view text)
{
    constexpr std::array<std::string_view, 7> keys{
        "boot_auto", "boot_cross", "boot_circle", "boot_square", "boot_triangle", "boot_start", "boot_select"};
    for (const auto key : keys) {
        const auto target = config_value(text, key);
        if (!target) continue;
        Button button;
        button.key = std::string(key);
        button.target = *target;
        for (int arg = 1; arg <= 16; ++arg) {
            const auto value = config_value(text, std::string(key) + "_arg" + std::to_string(arg));
            if (!value) break;
            button.args.push_back(*value);
        }
        evidence.osdmenu_buttons.push_back(std::move(button));
    }
    if (const auto prefer = config_value(text, "prefer_bbn")) evidence.osdmenu_prefer_bbn = parse_boolean(*prefer).value_or(false);
}

} // namespace

std::optional<std::string> config_value(std::string_view text, std::string_view key)
{
    std::size_t line = 0;
    while (line < text.size()) {
        auto end = text.find('\n', line);
        if (end == std::string_view::npos) end = text.size();
        std::size_t cursor = line;
        while (cursor < end && (text[cursor] == ' ' || text[cursor] == '\t' || text[cursor] == '\r')) ++cursor;
        if (cursor < end && text[cursor] != '#' && end - cursor >= key.size()) {
            std::size_t i = 0;
            while (i < key.size() && equal_nocase(text[cursor + i], key[i])) ++i;
            if (i == key.size()) {
                cursor += key.size();
                while (cursor < end && (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
                if (cursor < end && text[cursor] == '=') {
                    ++cursor;
                    while (cursor < end && (text[cursor] == ' ' || text[cursor] == '\t')) ++cursor;
                    std::size_t stop = cursor;
                    while (stop < end && text[stop] != '#' && text[stop] != '\r') ++stop;
                    while (stop > cursor && (text[stop - 1] == ' ' || text[stop - 1] == '\t')) --stop;
                    return std::string(text.substr(cursor, stop - cursor));
                }
            }
        }
        line = end == text.size() ? end : end + 1;
    }
    return std::nullopt;
}

std::optional<bool> parse_skip_hdd(std::string_view text)
{
    auto value = config_value(text, "OSDSYS_Skip_HDD");
    if (!value) value = config_value(text, "Skip_HDD");
    if (!value) return std::nullopt;
    return parse_boolean(*value);
}

AutoTarget parse_boot_auto(std::string_view text, std::string& value)
{
    const auto found = config_value(text, "boot_auto");
    value = found.value_or("");
    if (!found) return AutoTarget::none;
    if (*found == "$PSBBN") return AutoTarget::psbbn;
    if (*found == "$HOSDSYS") return AutoTarget::hosdsys;
    return AutoTarget::custom;
}

std::optional<std::string> find_fhdb_auto_target(std::string_view text)
{
    for (const char* key : {"LK_Auto_E1", "LK_Auto_E2", "LK_Auto_E3"}) {
        if (auto value = config_value(text, key)) return value;
    }
    return std::nullopt;
}

void classify(Report& report)
{
    const auto& e = report.evidence;
    auto set = [&report](std::string family, std::string confidence, std::string next) {
        report.family = std::move(family);
        report.confidence = std::move(confidence);
        report.next_stage = std::move(next);
    };
    using P = health::PointerState;
    if (e.pointer == P::half_empty) return set("Invalid pointer state", "certain", "none safely inferable");
    if (e.pointer == P::disabled) return set("No active payload", "certain", "none");
    if (e.pointer == P::unreadable || e.pointer == P::out_of_bounds) return set("Unreadable payload", "certain", "unknown");
    if (e.pointer == P::not_kelf) return set("Other / invalid KELF", "high", "unknown");
    if (e.pointer == P::unknown) return set("APA master not canonical", "certain", "unknown (repair the master first)");

    // Rule order is FHDB Manager's (Torii safety fix): explicit OSDMenu config
    // outranks partitions an older installation may have left behind.
    if (e.osdmenu_mbr_config && e.osdmenu_auto == AutoTarget::psbbn)
        return set("PSBBN / OSDMenu MBR", e.psbbn_boot ? "high" : "medium", "hdd0:__system:pfs:/p2lboot/osdboot.elf");
    if (e.osdmenu_mbr_config && e.osdmenu_auto == AutoTarget::hosdsys)
        return set("HDD-OSD / OSDMenu MBR", (e.hosdmenu_boot || e.hddosd_boot) ? "high" : "medium",
                   e.hosdmenu_boot ? "hdd0:__system:pfs:/osdmenu/hosdmenu.elf"
                   : e.hddosd_boot ? e.hddosd_path
                                   : "OSDMenu built-in $HOSDSYS target");
    if (e.osdmenu_mbr_config && e.osdmenu_auto == AutoTarget::custom)
        return set("Custom OSDMenu MBR", "high", e.osdmenu_auto_path.empty() ? "custom target" : e.osdmenu_auto_path);
    if (e.fhdb_config)
        return set("FHDB-compatible MBR", "medium",
                   e.legacy_osdmain ? "hdd0:__system:pfs:/osd/osdmain.elf"
                   : e.hddosd_boot  ? "hdd0:__system:pfs:/osd100 + __sysconf:/FMCB"
                                    : "hdd0:__sysconf:pfs:/FMCB (OSD stage unknown)");
    if (e.hosdmenu_boot || e.osdmenu_mbr_config)
        return set("HOSDMenu / OSDMenu MBR", "high", "hdd0:__system:pfs:/osdmenu/hosdmenu.elf");
    if (e.psbbn_boot || e.psbbn_partition_count >= 4)
        return set("PSBBN-compatible MBR", "medium", "hdd0:__system:pfs:/p2lboot/osdboot.elf");
    if (e.hddosd_boot || e.legacy_osdmain)
        return set("HDD-OSD-compatible MBR", "medium",
                   e.hddosd_boot ? e.hddosd_path : "hdd0:__system:pfs:/osd/osdmain.elf");
    set("Other / unknown KELF", "low", "not detected");
}

Report inspect(BlockDevice& device, const Options& options)
{
    Report report;
    auto& e = report.evidence;
    const auto health = health::assess(device);
    if (health.verdict == health::Verdict::blocked && !health.master_readable) {
        report.error = "The APA master could not be read";
        return report;
    }
    e.pointer = health.pointer;
    e.osd_start = health.osd_start;
    e.osd_sectors = health.osd_sectors;

    apa::Reader reader(device);
    const auto scan = reader.scan();
    if (!scan.ok()) report.notes.push_back("APA chain has errors; partition evidence may be incomplete");

    {
        PfsView sysconf(device, scan, "__sysconf");
        e.sysconf_present = sysconf.valid();
        if (sysconf.valid()) {
            for (const char* path : {"/FMCB/FREEHDB.CNF", "/FHDB/FREEHDB.CNF"}) {
                if (sysconf.exists(path)) {
                    e.fhdb_config = true;
                    e.fhdb_config_path = std::string("hdd0:__sysconf:pfs:") + path;
                    if (const auto text = sysconf.read_text(path)) e.fhdb_auto_path = find_fhdb_auto_target(*text).value_or("");
                    break;
                }
            }
            e.fhdb_boot_elf = sysconf.exists("/FMCB/BOOT.ELF") || sysconf.exists("/FHDB/BOOT.ELF");
            e.osdmenu_mbr_config = sysconf.exists("/osdmenu/OSDMBR.CNF");
            if (e.osdmenu_mbr_config) {
                if (const auto text = sysconf.read_text("/osdmenu/OSDMBR.CNF")) {
                    e.osdmenu_auto = parse_boot_auto(*text, e.osdmenu_auto_path);
                    scan_osdmenu_buttons(e, *text);
                } else {
                    report.notes.push_back("OSDMBR.CNF exists but could not be read");
                }
            }
        }
    }
    {
        PfsView system(device, scan, "__system");
        e.system_present = system.valid();
        if (system.valid()) {
            e.psbbn_boot = system.exists("/p2lboot/osdboot.elf");
            e.psbbn_kernel = system.exists("/p2lboot/vmlinux");
            e.p2lboot_files = system.list("/p2lboot");
            e.hosdmenu_boot = system.exists("/osdmenu/hosdmenu.elf");
            if (system.exists("/osd100/hosdsys.elf")) {
                e.hddosd_boot = true;
                e.hddosd_path = "hdd0:__system:pfs:/osd100/hosdsys.elf";
            } else if (system.exists("/osd100/OSDSYS_A.XLF")) {
                e.hddosd_boot = true;
                e.hddosd_path = "hdd0:__system:pfs:/osd100/OSDSYS_A.XLF";
            }
            e.legacy_osdmain = system.exists("/osd/osdmain.elf");
        }
    }
    for (const char* id : {"__linux.1", "__linux.4", "__linux.5", "__linux.6", "__linux.7", "__linux.8",
                           "__linux.9", "__contents"}) {
        if (std::any_of(scan.partitions.begin(), scan.partitions.end(),
                        [id](const apa::Partition& p) { return !p.is_sub() && p.id == id; }))
            ++e.psbbn_partition_count;
    }

    if (!options.memory_card_root.empty()) {
        const auto path = options.memory_card_root / "SYS-CONF" / "FREEMCB.CNF";
        if (const auto text = read_host_text(path)) {
            e.fmcb_config_path = path.string();
            e.fmcb_skip_hdd = parse_skip_hdd(*text);
        } else {
            report.notes.push_back("No SYS-CONF/FREEMCB.CNF in the memory-card folder");
        }
    }

    classify(report);

    if (e.fmcb_skip_hdd.value_or(false) && e.pointer == health::PointerState::valid)
        report.notes.push_back("FMCB has Skip_HDD enabled: a memory-card FMCB boot will not hand over to this HDD bootstrap");
    if (e.osdmenu_auto == AutoTarget::psbbn && !e.psbbn_kernel)
        report.notes.push_back("boot_auto is $PSBBN but __system/p2lboot/vmlinux is missing");
    if (e.osdmenu_mbr_config && e.osdmenu_auto == AutoTarget::none)
        report.notes.push_back("OSDMBR.CNF has no boot_auto entry");
    if (e.psbbn_partition_count >= 4 && e.osdmenu_mbr_config && e.osdmenu_auto != AutoTarget::psbbn)
        report.notes.push_back("PSBBN partitions exist but OSDMenu does not auto-boot them (stale install or intentional)");
    report.ok = true;
    return report;
}

std::string render(const Report& report)
{
    std::ostringstream out;
    if (!report.ok) {
        out << "Boot chain unavailable: " << report.error << '\n';
        return out.str();
    }
    const auto& e = report.evidence;
    out << "Boot family: " << report.family << " (confidence " << report.confidence << ")\n"
        << "Next stage:  " << report.next_stage << '\n'
        << "MBR program: " << health::pointer_name(e.pointer);
    if (e.pointer == health::PointerState::valid)
        out << " (sector 0x" << std::hex << e.osd_start << ", 0x" << e.osd_sectors << std::dec << " sectors)";
    out << "\n\n__sysconf: " << (e.sysconf_present ? "present" : "missing / not PFS") << '\n';
    if (e.fhdb_config)
        out << "  FHDB config: " << e.fhdb_config_path
            << (e.fhdb_auto_path.empty() ? "" : " -> LK_Auto " + e.fhdb_auto_path) << '\n';
    if (e.fhdb_boot_elf) out << "  FHDB BOOT.ELF present\n";
    if (e.osdmenu_mbr_config) {
        out << "  OSDMenu OSDMBR.CNF" << (e.osdmenu_prefer_bbn ? " (prefer_bbn = 1)" : "") << '\n';
        for (const auto& button : e.osdmenu_buttons) {
            out << "    " << button.key << " = " << (button.target.empty() ? "(none)" : button.target);
            for (const auto& arg : button.args) out << ' ' << arg;
            out << '\n';
        }
    }
    out << "__system:  " << (e.system_present ? "present" : "missing / not PFS") << '\n';
    if (e.psbbn_boot || e.psbbn_kernel || !e.p2lboot_files.empty()) {
        out << "  p2lboot:";
        for (const auto& name : e.p2lboot_files) out << ' ' << name;
        out << '\n';
    }
    if (e.hosdmenu_boot) out << "  HOSDMenu: osdmenu/hosdmenu.elf\n";
    if (e.hddosd_boot) out << "  HDD-OSD: " << e.hddosd_path << '\n';
    if (e.legacy_osdmain) out << "  legacy osd/osdmain.elf\n";
    out << "PSBBN partitions: " << e.psbbn_partition_count << " of 8\n";
    if (e.fmcb_skip_hdd) out << "FMCB Skip_HDD: " << (*e.fmcb_skip_hdd ? "ON" : "off") << " (" << e.fmcb_config_path << ")\n";
    for (const auto& note : report.notes) out << "[NOTE] " << note << '\n';
    return out.str();
}

} // namespace ps2hdd::bootchain
