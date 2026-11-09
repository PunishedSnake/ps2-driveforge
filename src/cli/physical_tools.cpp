#ifdef _WIN32

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/bootstrap_manifest.hpp"
#include "ps2hdd/bootstrap_provider.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"
#include "ps2hdd/fhdb_rescue_capture.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/http_client.hpp"
#include "ps2hdd/magicgate_service.hpp"
#include "ps2hdd/physical_apa_recovery.hpp"
#include "ps2hdd/physical_bootstrap_recovery.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/physical_game_deploy.hpp"
#include "ps2hdd/physical_partition_remove.hpp"
#include "ps2hdd/physical_write_guard.hpp"
#include "ps2hdd/boot_chain.hpp"
#include "ps2hdd/forensic_preview.hpp"
#include "ps2hdd/recovery_service.hpp"
#include "ps2hdd/structure_health.hpp"
#include "ps2hdd/version.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage()
{
    std::cerr
        << "ps2-driveforge-physical-tools " << ps2hdd::version::string << "\n\n"
        << "Read-only admission and recovery inspection:\n"
        << "  ps2-driveforge-physical-tools preflight <index>\n"
        << "  ps2-driveforge-physical-tools health <index>\n"
        << "  ps2-driveforge-physical-tools boot-chain <index> [--mc <memory-card-folder>]\n"
        << "  ps2-driveforge-physical-tools forensic-scan <index>\n"
        << "  ps2-driveforge-physical-tools capture-rescue <index> <artifact-dir>\n"
        << "      [--romver <text>] [--family <text>] [--confidence <text>]\n\n"
        << "HDL install:\n"
        << "  ps2-driveforge-physical-tools install-hdl <index> <game.iso> <title> <artifact-dir>\n"
        << "      --apply --confirm PhysicalDriveN [--cd] [--hidden]\n\n"
        << "Bootstrap provider staging/install:\n"
        << "  ps2-driveforge-physical-tools stage-provider <index> <manifest>\n"
        << "      [--keyset <file>] [--icvps2 <16hex>] [--icvps2-provenance <text>]\n"
        << "  ps2-driveforge-physical-tools install-provider <index> <manifest> <safety-dir>\n"
        << "      --apply --confirm PhysicalDriveN [--keyset <file>]\n"
        << "      [--icvps2 <16hex>] [--icvps2-provenance <text>]\n\n"
        << "APA/HDL remove:\n"
        << "  ps2-driveforge-physical-tools remove <index> <main-lba> <artifact-dir>\n"
        << "      --apply --confirm PhysicalDriveN\n\n"
        << "FHDB/bootstrap restore:\n"
        << "  ps2-driveforge-physical-tools restore-bootstrap <index> <artifact-dir>\n"
        << "      --apply --confirm PhysicalDriveN [--safety-dir <dir>]\n"
        << "  ps2-driveforge-physical-tools disable-bootstrap <index> <safety-dir>\n"
        << "      --apply --confirm PhysicalDriveN   (HDDMBR + Rescue Capsule, then clear osdStart/osdSize)\n\n"
        << "Exceptional APA recovery:\n"
        << "  ps2-driveforge-physical-tools repair-master <index> <artifact-dir>\n"
        << "      --apply --confirm PhysicalDriveN\n"
        << "  ps2-driveforge-physical-tools repair-forensic <index> <map-index> <artifact-dir>\n"
        << "      --apply --confirm PhysicalDriveN [--allow-manual]\n\n"
        << "Drive migration (copies only APA-allocated space, master written last):\n"
        << "  ps2-driveforge-physical-tools migrate <src-index> <dst-index> [--apply --confirm PhysicalDriveDST]\n"
        << "  ps2-driveforge-physical-tools backup <src-index> <new-image> [<size>]   (sparse image, default = source size)\n"
        << "  ps2-driveforge-physical-tools restore-image <image> <dst-index> [--apply --confirm PhysicalDriveDST]\n\n"
        << "Provider network/crypto staging completes before any writable raw-disk handle is opened.\n"
        << "Physical mutations require an exact target confirmation and host-side recovery artifacts.\n";
}

bool parse_unsigned(std::string_view text, unsigned& value)
{
    value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

bool parse_u32(std::string_view text, std::uint32_t& value)
{
    value = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

bool has_flag(std::span<char*> args, std::string_view flag)
{
    for (const auto* arg : args) {
        if (arg != nullptr && std::string_view(arg) == flag) {
            return true;
        }
    }
    return false;
}

std::string value_after(std::span<char*> args, std::string_view flag)
{
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] != nullptr && std::string_view(args[i]) == flag) {
            return args[i + 1] == nullptr ? std::string{} : std::string(args[i + 1]);
        }
    }
    return {};
}

bool confirm_target(std::span<char*> args, unsigned index, std::string& error)
{
    if (!has_flag(args, "--apply")) {
        error = "physical mutation requires --apply";
        return false;
    }
    const auto confirmation = value_after(args, "--confirm");
    const auto expected = "PhysicalDrive" + std::to_string(index);
    if (confirmation != expected) {
        error = "physical mutation requires --confirm " + expected;
        return false;
    }
    return true;
}

std::string digest_hex(const ps2hdd::crypto::Sha256Digest& digest)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        out << std::setw(2) << std::to_integer<unsigned>(byte);
    }
    return out.str();
}

bool read_text_bounded(const std::filesystem::path& path,
                       std::size_t max_bytes,
                       std::string& text,
                       std::string& error)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "Could not open local file: " + path.string();
        return false;
    }
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > max_bytes) {
        error = "Local file exceeds its bounded read limit: " + path.string();
        return false;
    }
    text.resize(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        error = "Could not read local file completely: " + path.string();
        return false;
    }
    return true;
}

int hex_nibble(char ch) noexcept
{
    if (ch >= '0' && ch <= '9') return static_cast<int>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<int>(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'F') return static_cast<int>(ch - 'A') + 10;
    return -1;
}

bool parse_icvps2(std::string_view text,
                  ps2hdd::magicgate::cipher::Block& out,
                  std::string& error)
{
    if (text.empty()) return false;
    if (text.size() != out.size() * 2U) {
        error = "ICVPS2 must contain exactly 16 hexadecimal characters (8 bytes)";
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = hex_nibble(text[i * 2U]);
        const int lo = hex_nibble(text[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            error = "ICVPS2 contains a non-hexadecimal character";
            return false;
        }
        out[i] = static_cast<std::byte>((hi << 4) | lo);
    }
    return true;
}

struct ProviderStageResult {
    bool ok{};
    std::string error;
    std::string product;
    std::string source_sha256;
    std::string payload_sha256;
    ps2hdd::bootstrap::FrozenInstallInput frozen;
};

ProviderStageResult stage_provider(unsigned index,
                                   const std::filesystem::path& manifest_path,
                                   std::span<char*> args)
{
    ProviderStageResult result;
    std::string manifest_text;
    if (!read_text_bounded(manifest_path, 128U * 1024U, manifest_text, result.error)) {
        return result;
    }
    const auto parsed = ps2hdd::bootstrap::parse_provider_manifest(manifest_text);
    if (!parsed.ok) {
        result.error = parsed.error;
        if (parsed.line != 0U) result.error += " at line " + std::to_string(parsed.line);
        return result;
    }

    ps2hdd::PhysicalDrive disk(index);
    if (!disk.is_open()) {
        result.error = "Could not open PhysicalDrive" + std::to_string(index) + " read-only for provider staging";
        return result;
    }
    const auto admission = ps2hdd::physical_write::authorize(disk);
    if (!admission.ok) {
        result.error = "Physical provider preflight refused the target: " + admission.error;
        return result;
    }
    const auto target_fingerprint = digest_hex(admission.authorization.identity.digest);

    auto http = ps2hdd::make_platform_http_client();
    if (!http) {
        result.error = "No native HTTPS provider transport is available on this platform";
        return result;
    }
    const auto acquired = ps2hdd::bootstrap::acquire(*http, parsed.artifact);
    if (!acquired.ok) {
        result.error = acquired.error;
        return result;
    }

    ps2hdd::magicgate::MagicGateHostService magicgate;
    if (parsed.artifact.magicgate_policy == ps2hdd::bootstrap::MagicGatePolicy::verify_kelf ||
        parsed.artifact.magicgate_policy == ps2hdd::bootstrap::MagicGatePolicy::sign_plaintext) {
        const auto keyset_path = value_after(args, "--keyset");
        if (keyset_path.empty()) {
            result.error = "This provider policy requires --keyset <local-file>";
            return result;
        }
        std::string keyset_text;
        if (!read_text_bounded(keyset_path, magicgate.limits().max_keyset_bytes,
                               keyset_text, result.error)) {
            return result;
        }
        std::string key_error;
        if (!magicgate.load_keyset_text(
                keyset_text, "local file: " + keyset_path, key_error)) {
            result.error = key_error;
            return result;
        }
    }

    const auto icvps2_hex = value_after(args, "--icvps2");
    if (!icvps2_hex.empty()) {
        ps2hdd::magicgate::cipher::Block icv{};
        if (!parse_icvps2(icvps2_hex, icv, result.error)) return result;
        const auto provenance = value_after(args, "--icvps2-provenance");
        if (provenance.empty()) {
            result.error = "ICVPS2 evidence requires --icvps2-provenance <text>";
            return result;
        }
        std::string evidence_error;
        if (!magicgate.load_icvps2(icv, provenance, evidence_error)) {
            result.error = evidence_error;
            return result;
        }
    }

    if (parsed.artifact.magicgate_policy == ps2hdd::bootstrap::MagicGatePolicy::sign_plaintext) {
        result.error = "Provider manifest requests MagicGate signing, but generic manifests do not serialize a DiskKelfSignPlan; use a typed provider strategy or an already signed/verified KELF";
        return result;
    }

    const auto prepared = ps2hdd::bootstrap::prepare(acquired, &magicgate);
    if (!prepared.ok) {
        result.error = prepared.error;
        return result;
    }
    result.frozen = ps2hdd::bootstrap::freeze_install_input(prepared, target_fingerprint);
    if (!result.frozen.ok) {
        result.error = result.frozen.error;
        return result;
    }
    std::string frozen_error;
    if (!ps2hdd::bootstrap::validate_frozen_payload(result.frozen, frozen_error)) {
        result.error = frozen_error;
        result.frozen = {};
        return result;
    }

    result.product = std::string(ps2hdd::bootstrap::strategy_for(parsed.artifact.family).name);
    result.source_sha256 = prepared.source_sha256;
    result.payload_sha256 = prepared.payload_sha256;
    result.ok = true;
    return result;
}

void print_provider_stage(unsigned index, const ProviderStageResult& staged)
{
    std::cout << "Bootstrap provider staged read-only\n"
              << "  target:       PhysicalDrive" << index << "\n"
              << "  product:      " << staged.product << "\n"
              << "  provider:     " << staged.frozen.provider_id << "\n"
              << "  version:      " << staged.frozen.immutable_version << "\n"
              << "  provenance:   " << staged.frozen.provenance << "\n"
              << "  source SHA:   " << staged.source_sha256 << "\n"
              << "  payload SHA:  " << staged.payload_sha256 << "\n"
              << "  payload:      " << staged.frozen.payload.size() << " bytes\n"
              << "  sectors:      " << staged.frozen.payload_sector_count << "\n"
              << "  __mbr start:  " << staged.frozen.program_start_sector << "\n"
              << "  target SHA:   " << staged.frozen.target_fingerprint << "\n"
              << "  MagicGate:    " << (staged.frozen.magicgate_verified ? "verified" : "not required")
              << (staged.frozen.magicgate_signed ? ", signed" : "") << "\n";
}

int provider_stage_command(std::span<char*> args, bool install)
{
    const std::size_t minimum = install ? 7U : 4U;
    if (args.size() < minimum) {
        usage();
        return 2;
    }

    unsigned index = 0;
    if (!parse_unsigned(args[2], index)) {
        std::cerr << "Invalid physical drive index\n";
        return 2;
    }
    if (install) {
        std::string confirmation_error;
        if (!confirm_target(args, index, confirmation_error)) {
            std::cerr << confirmation_error << "\n";
            return 2;
        }
    }

    const auto staged = stage_provider(index, std::filesystem::path(args[3]), args);
    if (!staged.ok) {
        std::cerr << "Bootstrap provider staging refused: " << staged.error << "\n";
        return 1;
    }
    print_provider_stage(index, staged);
    if (!install) {
        std::cout << "No target writes were performed.\n";
        return 0;
    }

    ps2hdd::PhysicalBootstrapInstallOptions options;
    options.safety_directory = std::filesystem::path(args[4]);
    const auto result = ps2hdd::install_bootstrap_to_physical(index, staged.frozen, options);
    if (!result.ok) {
        std::cerr << "Physical bootstrap provider install failed: " << result.error << "\n";
        if (!result.rescue_capsule_path.empty()) {
            std::cerr << "Rescue Capsule: " << result.rescue_capsule_path.string() << "\n";
        }
        if (!result.hddmbr_path.empty()) {
            std::cerr << "HDDMBR: " << result.hddmbr_path.string() << "\n";
        }
        return result.partial ? 3 : 1;
    }

    std::cout << "Physical bootstrap provider install complete\n"
              << "  Rescue Capsule: " << result.rescue_capsule_path.string() << "\n"
              << "  HDDMBR:         " << result.hddmbr_path.string() << "\n"
              << "  cold master:    " << (result.cold_master_verified ? "verified" : "not verified") << "\n"
              << "  cold payload:   " << (result.cold_payload_verified ? "verified" : "not verified") << "\n"
              << "  locked Windows volumes: " << result.locked_volume_count << "\n";
    return 0;
}

int preflight(unsigned index)
{
    ps2hdd::PhysicalDrive disk(index);
    if (!disk.is_open()) {
        std::cerr << "Could not open PhysicalDrive" << index
                  << " read-only, Win32 error " << disk.open_error() << "\n";
        return 1;
    }

    const auto admission = ps2hdd::physical_write::authorize(disk);
    if (!admission.ok) {
        std::cerr << "Physical write admission: REFUSED\nReason: " << admission.error << "\n";
        return 1;
    }

    ps2hdd::apa::Reader reader(disk);
    const auto scan = reader.scan();
    std::cout << "PhysicalDrive" << index << "\n"
              << "  size:        " << disk.size_bytes() << " bytes\n"
              << "  APA version: " << admission.authorization.apa_version << "\n"
              << "  APA headers: " << admission.authorization.apa_header_count << "\n"
              << "  main/sub:    ";
    std::size_t mains = 0;
    std::size_t subs = 0;
    for (const auto& partition : scan.partitions) {
        partition.is_sub() ? ++subs : ++mains;
    }
    std::cout << mains << "/" << subs << "\n"
              << "  identity:    " << digest_hex(admission.authorization.identity.digest) << "\n"
              << "  write gate:  eligible for guarded RW lease\n";
    return 0;
}

int forensic_scan(unsigned index)
{
    ps2hdd::PhysicalDrive disk(index);
    if (!disk.is_open()) {
        std::cerr << "Could not open PhysicalDrive" << index
                  << " read-only, Win32 error " << disk.open_error() << "\n";
        return 1;
    }

    // Raw forensic scanning is intentionally available when normal preflight is
    // not. The entire point is to acquire evidence from disks whose canonical
    // APA chain can no longer be trusted. This command never opens an RW handle.
    const auto scan = ps2hdd::forensic::scan_apa(disk);
    std::cout << "Forensic APA scan for PhysicalDrive" << index << "\n"
              << "  status:           " << (scan.ok ? "complete" : "failed") << "\n"
              << "  total sectors:    " << scan.total_sectors << "\n"
              << "  candidate nodes:  " << scan.nodes.size() << "\n"
              << "  candidate maps:   " << scan.maps.size() << "\n"
              << "  grid reads:       " << scan.grid_reads << "\n"
              << "  reference reads:  " << scan.reference_reads << "\n"
              << "  unreadable reads: " << scan.unreadable_reads << "\n"
              << "  truncated:        " << (scan.truncated ? "YES" : "no") << "\n";
    if (!scan.error.empty()) {
        std::cout << "  diagnostic:       " << scan.error << "\n";
    }

    for (std::size_t i = 0; i < scan.maps.size(); ++i) {
        const auto& map = scan.maps[i];
        const auto plan = ps2hdd::forensic::build_repair_plan(scan, i);
        std::cout << "\nMap " << i << " (" << ps2hdd::forensic::map_name(map.kind) << ")\n"
                  << "  nodes:        " << map.order.size() << "\n"
                  << "  reciprocal:   " << map.reciprocal_links << "\n"
                  << "  inferred:     " << map.inferred_links << "\n"
                  << "  conflicts:    " << map.conflicts << "\n"
                  << "  overlaps:     " << map.overlaps << "\n"
                  << "  confidence:   " << map.confidence << "\n"
                  << "  repairable:   " << (map.repairable ? "yes" : "no") << "\n"
                  << "  patches:      " << plan.patches.size() << "\n"
                  << "  auto-safe:    " << (plan.automatic_safe ? "YES" : "no") << "\n"
                  << "  manual-only:  "
                  << (!plan.automatic_safe && plan.manual_allowed ? "yes" : "no") << "\n"
                  << ps2hdd::forensic::render_preview(ps2hdd::forensic::preview(scan, i));
    }

    return scan.ok && !scan.truncated ? 0 : 1;
}

int capture_rescue(std::span<char*> args)
{
    if (args.size() < 4) {
        usage();
        return 2;
    }
    unsigned index = 0;
    if (!parse_unsigned(args[2], index)) {
        std::cerr << "Invalid physical drive index\n";
        return 2;
    }

    ps2hdd::PhysicalDrive disk(index);
    if (!disk.is_open()) {
        std::cerr << "Could not open PhysicalDrive" << index
                  << " read-only, Win32 error " << disk.open_error() << "\n";
        return 1;
    }

    ps2hdd::fhdb::RescueCaptureOptions options;
    options.romver = value_after(args, "--romver");
    options.family = value_after(args, "--family");
    options.confidence = value_after(args, "--confidence");
    const auto captured = ps2hdd::fhdb::capture_rescue_image(disk, options);
    if (!captured.ok) {
        std::cerr << "Rescue Capsule capture failed: " << captured.error << "\n";
        return 1;
    }

    const auto saved = ps2hdd::fhdb::save_hddrescue(std::filesystem::path(args[3]), captured);
    if (!saved.ok) {
        std::cerr << "Captured Rescue Capsule but could not persist it safely: " << saved.error << "\n";
        return 1;
    }

    std::cout << "FHDB Rescue Capsule captured read-only\n"
              << "  source:     PhysicalDrive" << index << "\n"
              << "  artifact:   " << saved.path.string() << (saved.reused ? " (reused)" : "") << "\n"
              << "  payload:    " << captured.info.payload_bytes << " bytes\n"
              << "  osdStart:   " << captured.info.payload_start << "\n"
              << "  osdSize:    " << captured.info.payload_sectors << " sectors\n"
              << "  valid KELF: "
              << ((captured.info.flags & ps2hdd::fhdb::kRescueFlagValidKelf) != 0 ? "yes" : "no")
              << "\n";
    return 0;
}

int install_hdl(std::span<char*> args)
{
    if (args.size() < 8) {
        usage();
        return 2;
    }

    unsigned index = 0;
    if (!parse_unsigned(args[2], index)) {
        std::cerr << "Invalid physical drive index\n";
        return 2;
    }
    std::string confirmation_error;
    if (!confirm_target(args, index, confirmation_error)) {
        std::cerr << confirmation_error << "\n";
        return 2;
    }

    // Braces are intentional. The parenthesized spelling is the classic C++
    // "most vexing parse" under MSVC here and becomes a function declaration.
    ps2hdd::FileBlockDevice iso{std::filesystem::path(args[3])};
    if (!iso.is_open()) {
        std::cerr << "Could not open game ISO: " << args[3] << "\n";
        return 1;
    }

    ps2hdd::PhysicalGameDeployOptions options;
    options.game.hdl.title = args[4];
    options.game.hdl.media = has_flag(args, "--cd")
                                 ? ps2hdd::hdl::MediaType::cd
                                 : ps2hdd::hdl::MediaType::dvd;
    options.game.hdl.hidden = has_flag(args, "--hidden");
    options.artifact_directory = std::filesystem::path(args[5]);

    std::vector<ps2hdd::opl::FetchedAsset> no_assets;
    std::string last_phase;
    const auto result = ps2hdd::deploy_game_to_physical(
        index, iso, no_assets, options,
        [&](const ps2hdd::hdl::InstallProgress& progress) {
            if (progress.phase != last_phase) {
                last_phase = std::string(progress.phase);
                std::cout << "[" << last_phase << "]\n";
            }
            if (progress.total_bytes != 0) {
                std::cout << "  " << progress.copied_bytes << "/" << progress.total_bytes << " bytes\r";
            }
        });
    std::cout << "\n";

    if (!result.ok) {
        std::cerr << "Physical HDL install failed: " << result.error << "\n"
                  << "HDDMBR: " << result.hddmbr_path.string() << "\n"
                  << "Journal: " << result.mutation_journal_path.string() << "\n";
        return result.partial ? 3 : 1;
    }

    std::cout << "Physical HDL install complete\n"
              << "  partition: " << result.deployment.game.partition_id << "\n"
              << "  startup:   " << result.deployment.game.startup << "\n"
              << "  main LBA:  " << result.deployment.game.main_start_lba << "\n"
              << "  subs:      " << result.deployment.game.sub_count << "\n"
              << "  HDDMBR:    " << result.hddmbr_path.string() << "\n"
              << "  journal:   " << result.mutation_journal_path.string() << "\n"
              << "  locked Windows volumes: " << result.locked_volume_count << "\n";
    return 0;
}

int remove_partition(std::span<char*> args)
{
    if (args.size() < 7) {
        usage();
        return 2;
    }

    unsigned index = 0;
    std::uint32_t main_lba = 0;
    if (!parse_unsigned(args[2], index) || !parse_u32(args[3], main_lba)) {
        std::cerr << "Invalid physical drive index or main LBA\n";
        return 2;
    }
    std::string confirmation_error;
    if (!confirm_target(args, index, confirmation_error)) {
        std::cerr << confirmation_error << "\n";
        return 2;
    }

    ps2hdd::PhysicalPartitionRemoveOptions options;
    options.artifact_directory = std::filesystem::path(args[4]);
    const auto result = ps2hdd::remove_partition_from_physical(index, main_lba, options);
    if (!result.ok) {
        std::cerr << "Physical APA removal failed: " << result.error << "\n"
                  << "HDDMBR: " << result.hddmbr_path.string() << "\n"
                  << "Journal: " << result.mutation_journal_path.string() << "\n"
                  << "Recovery pending: " << (result.recovery_pending ? "YES" : "no") << "\n";
        return result.recovery_pending ? 3 : 1;
    }

    std::cout << "Physical APA removal complete\n"
              << "  partition: " << result.removal.partition_id << "\n"
              << "  detached headers: " << result.removal.removed_headers << "\n"
              << "  rewritten neighbours: " << result.removal.rewritten_headers << "\n"
              << "  freed logical bytes: " << result.removal.freed_bytes << "\n"
              << "  HDDMBR: " << result.hddmbr_path.string() << "\n"
              << "  journal: " << result.mutation_journal_path.string() << "\n";
    return 0;
}

int restore_bootstrap(std::span<char*> args)
{
    if (args.size() < 7) {
        usage();
        return 2;
    }

    unsigned index = 0;
    if (!parse_unsigned(args[2], index)) {
        std::cerr << "Invalid physical drive index\n";
        return 2;
    }
    std::string confirmation_error;
    if (!confirm_target(args, index, confirmation_error)) {
        std::cerr << confirmation_error << "\n";
        return 2;
    }

    ps2hdd::PhysicalBootstrapRestoreOptions options;
    options.artifact_directory = std::filesystem::path(args[3]);
    const auto safety = value_after(args, "--safety-dir");
    options.safety_directory = safety.empty()
                                   ? options.artifact_directory
                                   : std::filesystem::path(safety);

    const auto result = ps2hdd::restore_bootstrap_to_physical(index, options);
    if (!result.ok) {
        std::cerr << "Physical bootstrap restore failed: " << result.error << "\n";
        if (!result.restore.safety_backup_path.empty()) {
            std::cerr << "Safety HDDMBR: " << result.restore.safety_backup_path.string() << "\n";
        }
        return result.partial ? 3 : 1;
    }

    std::cout << "Physical bootstrap restore complete\n"
              << "  source artifact: " << result.restore.source_path.string() << "\n"
              << "  safety HDDMBR:   " << result.restore.safety_backup_path.string() << "\n"
              << "  osdStart:        " << result.restore.payload_start << "\n"
              << "  osdSize:         " << result.restore.payload_sectors << " sectors\n"
              << "  payload bytes:   " << result.restore.payload_bytes << "\n"
              << "  cold master:     " << (result.cold_master_verified ? "verified" : "not verified") << "\n"
              << "  cold payload:    " << (result.cold_payload_verified ? "verified/not-applicable" : "not verified") << "\n"
              << "  locked Windows volumes: " << result.locked_volume_count << "\n";
    return 0;
}

int repair_master(std::span<char*> args)
{
    if (args.size() < 6) {
        usage();
        return 2;
    }
    unsigned index = 0;
    if (!parse_unsigned(args[2], index)) {
        std::cerr << "Invalid physical drive index\n";
        return 2;
    }
    std::string confirmation_error;
    if (!confirm_target(args, index, confirmation_error)) {
        std::cerr << confirmation_error << "\n";
        return 2;
    }

    ps2hdd::PhysicalApaRecoveryOptions options;
    options.artifact_directory = std::filesystem::path(args[3]);
    const auto result = ps2hdd::repair_master_header_on_physical(index, options);
    if (!result.ok) {
        std::cerr << "Physical APA master repair failed: " << result.error << "\n";
        if (!result.repair.snapshot_path.empty()) {
            std::cerr << "HDDRAW: " << result.repair.snapshot_path.string() << "\n";
        }
        return result.partial ? 3 : 1;
    }

    std::cout << "Physical APA master repair complete\n"
              << "  HDDRAW:       " << result.repair.snapshot_path.string() << "\n"
              << "  cold master:  " << (result.cold_master_verified ? "verified" : "not verified") << "\n"
              << "  full APA scan: " << (result.cold_apa_clean ? "clean" : "still has other damage") << "\n"
              << "  locked Windows volumes: " << result.locked_volume_count << "\n";
    return 0;
}


bool parse_size(std::string_view text, std::uint64_t& bytes)
{
    std::uint64_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || value == 0) return false;
    const std::string_view unit(end, static_cast<std::size_t>(text.data() + text.size() - end));
    std::uint64_t scale = 1;
    if (unit == "K" || unit == "KiB") scale = 1ULL << 10;
    else if (unit == "M" || unit == "MiB") scale = 1ULL << 20;
    else if (unit == "G" || unit == "GiB") scale = 1ULL << 30;
    else if (unit == "T" || unit == "TiB") scale = 1ULL << 40;
    else if (!unit.empty()) return false;
    bytes = value * scale;
    return true;
}

int run_migration(const ps2hdd::recovery_service::Source& source, const ps2hdd::recovery_service::MigrationTarget& target,
                  bool apply)
{
    namespace rs = ps2hdd::recovery_service;
    const auto planned = rs::plan_migration(source, target);
    if (!planned.ok) {
        std::cerr << "Migration refused: " << planned.error << '\n';
        return 1;
    }
    std::cout << planned.text << '\n';
    if (!apply) {
        std::cout << "Dry run; add --apply to copy.\n";
        return 0;
    }
    int last = -1;
    const auto outcome = rs::migrate(source, target, planned.plan, [&last](std::uint64_t done, std::uint64_t total) {
        const int pct = total == 0 ? 100 : static_cast<int>(done * 100U / total);
        if (pct != last) {
            last = pct;
            std::cerr << "\r  copying " << pct << "%" << std::flush;
        }
    });
    std::cerr << '\n';
    if (!outcome.warning.empty()) std::cout << "[WARN] " << outcome.warning << '\n';
    if (!outcome.ok) {
        std::cerr << "Migration failed: " << outcome.error << '\n';
        return outcome.partial ? 3 : 1;
    }
    std::cout << outcome.summary << '\n';
    return 0;
}

int health(unsigned index)
{
    ps2hdd::PhysicalDrive disk(index);
    if (!disk.is_open()) {
        std::cerr << "Could not open PhysicalDrive" << index
                  << " read-only, Win32 error " << disk.open_error() << "\n";
        return 1;
    }
    const auto report = ps2hdd::health::assess(disk);
    std::cout << "Structure health for PhysicalDrive" << index << " (read-only)\n"
              << ps2hdd::health::render(report);
    return report.verdict == ps2hdd::health::Verdict::healthy ? 0 : 4;
}

int disable_bootstrap(std::span<char*> args)
{
    if (args.size() < 6) {
        usage();
        return 2;
    }
    unsigned index = 0;
    if (!parse_unsigned(args[2], index)) {
        std::cerr << "Invalid physical drive index\n";
        return 2;
    }
    std::string confirmation_error;
    if (!confirm_target(args, index, confirmation_error)) {
        std::cerr << confirmation_error << "\n";
        return 2;
    }
    const auto result = ps2hdd::disable_bootstrap_on_physical(index, std::filesystem::path(args[3]));
    if (!result.ok) {
        std::cerr << result.error << "\n";
        if (!result.disable.hddmbr_path.empty()) std::cerr << "HDDMBR: " << result.disable.hddmbr_path.string() << "\n";
        if (!result.disable.warning.empty()) std::cerr << "[WARN] " << result.disable.warning << "\n";
        return result.partial ? 3 : 1;
    }
    if (result.disable.already_disabled) {
        std::cout << "Bootstrap pointer is already disabled; nothing was written\n";
        return 0;
    }
    std::cout << "Bootstrap disabled and cold-verified\n"
              << "  previous pointer: start 0x" << std::hex << result.disable.previous_start << ", size 0x"
              << result.disable.previous_sectors << std::dec << "\n"
              << "  HDDMBR:  " << result.disable.hddmbr_path.string() << "\n"
              << "  rescue:  " << (result.disable.rescue_path.empty() ? std::string("not available")
                                                                       : result.disable.rescue_path.string()) << "\n"
              << "  locked Windows volumes: " << result.locked_volume_count << "\n";
    if (!result.disable.warning.empty()) std::cout << "[WARN] " << result.disable.warning << "\n";
    return 0;
}

int repair_forensic(std::span<char*> args)
{
    if (args.size() < 7) {
        usage();
        return 2;
    }

    unsigned index = 0;
    unsigned raw_map_index = 0;
    if (!parse_unsigned(args[2], index) || !parse_unsigned(args[3], raw_map_index)) {
        std::cerr << "Invalid physical drive index or forensic map index\n";
        return 2;
    }
    std::string confirmation_error;
    if (!confirm_target(args, index, confirmation_error)) {
        std::cerr << confirmation_error << "\n";
        return 2;
    }

    ps2hdd::PhysicalDrive disk(index);
    if (!disk.is_open()) {
        std::cerr << "Could not open PhysicalDrive" << index << " read-only for forensic scan\n";
        return 1;
    }
    const auto scan = ps2hdd::forensic::scan_apa(disk);
    const auto map_index = static_cast<std::size_t>(raw_map_index);
    if (!scan.ok || scan.truncated || map_index >= scan.maps.size()) {
        std::cerr << "Forensic scan is incomplete or does not contain map " << map_index << "\n";
        return 1;
    }
    const auto plan = ps2hdd::forensic::build_repair_plan(scan, map_index);
    const bool allow_manual = has_flag(args, "--allow-manual");
    if (!plan.automatic_safe && !(allow_manual && plan.manual_allowed)) {
        std::cerr << "Selected forensic plan is not automatic-safe";
        if (plan.manual_allowed) {
            std::cerr << "; explicit --allow-manual is required for this developer path";
        }
        std::cerr << "\n";
        return 1;
    }

    ps2hdd::PhysicalApaRecoveryOptions options;
    options.artifact_directory = std::filesystem::path(args[4]);
    const auto result = ps2hdd::repair_forensic_topology_on_physical(
        index, scan, plan, options, allow_manual);
    if (!result.ok) {
        std::cerr << "Physical forensic APA repair failed: " << result.error << "\n";
        if (!result.repair.snapshot_path.empty()) {
            std::cerr << "HDDMETA: " << result.repair.snapshot_path.string() << "\n";
        }
        if (!result.repair.forensic_report_path.empty()) {
            std::cerr << "FORENSIC: " << result.repair.forensic_report_path.string() << "\n";
        }
        return result.partial ? 3 : 1;
    }

    std::cout << "Physical forensic APA repair complete\n"
              << "  map:          " << map_index << " ("
              << ps2hdd::forensic::map_name(scan.maps[map_index].kind) << ")\n"
              << "  patches:      " << plan.patches.size() << "\n"
              << "  writes:       " << result.repair.writes_completed << "\n"
              << "  HDDMETA:      " << result.repair.snapshot_path.string() << "\n"
              << "  FORENSIC:     " << result.repair.forensic_report_path.string() << "\n"
              << "  cold touched: " << (result.cold_touched_set_verified ? "verified" : "not verified") << "\n"
              << "  full APA scan: " << (result.cold_apa_clean ? "clean" : "still has other damage") << "\n"
              << "  locked Windows volumes: " << result.locked_volume_count << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    const std::string_view command = argv[1];

    if (command == "--version") {
        std::cout << ps2hdd::version::string << "\n";
        return 0;
    }
    if (command == "preflight") {
        if (argc != 3) {
            usage();
            return 2;
        }
        unsigned index = 0;
        if (!parse_unsigned(argv[2], index)) {
            std::cerr << "Invalid physical drive index\n";
            return 2;
        }
        return preflight(index);
    }
    if (command == "health") {
        unsigned index = 0;
        if (argc != 3 || !parse_unsigned(argv[2], index)) {
            usage();
            return 2;
        }
        return health(index);
    }
    if (command == "migrate" || command == "restore-image") {
        if (argc < 4) {
            usage();
            return 2;
        }
        ps2hdd::recovery_service::Source source;
        unsigned src = 0, dst = 0;
        if (command == "migrate") {
            if (!parse_unsigned(argv[2], src)) { usage(); return 2; }
            source.physical = src;
        } else {
            source.image = argv[2];
        }
        if (!parse_unsigned(argv[3], dst)) { usage(); return 2; }
        ps2hdd::recovery_service::MigrationTarget target;
        target.physical = dst;
        const bool apply = has_flag(args, "--apply");
        if (apply) {
            std::string error;
            if (!confirm_target(args, dst, error)) {
                std::cerr << error << " (the TARGET drive)\n";
                return 2;
            }
        }
        return run_migration(source, target, apply);
    }
    if (command == "backup") {
        unsigned src = 0;
        if (argc < 4 || !parse_unsigned(argv[2], src)) { usage(); return 2; }
        ps2hdd::recovery_service::Source source;
        source.physical = src;
        ps2hdd::recovery_service::MigrationTarget target;
        target.image = argv[3];
        if (argc >= 5 && argv[4][0] != '-') {
            if (!parse_size(argv[4], target.image_bytes)) { usage(); return 2; }
        } else {
            ps2hdd::PhysicalDrive disk(src);
            target.image_bytes = disk.is_open() ? disk.size_bytes() : 0;
        }
        return run_migration(source, target, true);
    }
    if (command == "boot-chain") {
        unsigned index = 0;
        if ((argc != 3 && argc != 5) || !parse_unsigned(argv[2], index)) {
            usage();
            return 2;
        }
        ps2hdd::PhysicalDrive disk(index);
        if (!disk.is_open()) {
            std::cerr << "Could not open PhysicalDrive" << index << " read-only\n";
            return 1;
        }
        ps2hdd::bootchain::Options options;
        if (argc == 5 && std::string_view(argv[3]) == "--mc") options.memory_card_root = argv[4];
        const auto report = ps2hdd::bootchain::inspect(disk, options);
        std::cout << ps2hdd::bootchain::render(report);
        return report.ok ? 0 : 1;
    }
    if (command == "disable-bootstrap") {
        return disable_bootstrap(args);
    }
    if (command == "forensic-scan") {
        if (argc != 3) {
            usage();
            return 2;
        }
        unsigned index = 0;
        if (!parse_unsigned(argv[2], index)) {
            std::cerr << "Invalid physical drive index\n";
            return 2;
        }
        return forensic_scan(index);
    }
    if (command == "capture-rescue") {
        return capture_rescue(args);
    }
    if (command == "stage-provider") {
        return provider_stage_command(args, false);
    }
    if (command == "install-provider") {
        return provider_stage_command(args, true);
    }
    if (command == "install-hdl") {
        return install_hdl(args);
    }
    if (command == "remove") {
        return remove_partition(args);
    }
    if (command == "restore-bootstrap") {
        return restore_bootstrap(args);
    }
    if (command == "repair-master") {
        return repair_master(args);
    }
    if (command == "repair-forensic") {
        return repair_forensic(args);
    }

    usage();
    return 2;
}

#endif