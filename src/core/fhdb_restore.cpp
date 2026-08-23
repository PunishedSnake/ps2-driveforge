#include "ps2hdd/fhdb_restore.hpp"

#include "ps2hdd/apa.hpp"
#include "ps2hdd/fhdb_artifacts.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace ps2hdd::fhdb {
namespace {

constexpr std::size_t kApaChecksumOffset = 0x000;
constexpr std::size_t kApaStartOffset = 0x040;
constexpr std::size_t kApaLengthOffset = 0x044;
constexpr std::size_t kApaOsdStartOffset = 0x130;
constexpr std::size_t kApaOsdSizeOffset = 0x134;
constexpr std::uint32_t kMbrPayloadStart = 0x2000U;
constexpr std::uint32_t kSectorSize = 512U;
constexpr std::uint32_t kMaxMbrPayloadBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxRescueFileBytes =
    kRescueMetadataBytes + kRescueApaHeaderBytes + kMaxMbrPayloadBytes;

[[nodiscard]] std::uint32_t load_u32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

void store_u32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::uint32_t raw_apa_checksum(
    std::span<const std::byte, kRescueApaHeaderBytes> header) noexcept
{
    std::uint32_t sum = 0;
    for (std::size_t word = 1; word < 256; ++word) {
        sum += load_u32(header.data() + static_cast<std::ptrdiff_t>(word * 4));
    }
    return sum;
}

[[nodiscard]] bool read_current_master(
    BlockDevice& disk,
    std::array<std::byte, kRescueApaHeaderBytes>& master,
    std::string& error)
{
    if (disk.size_bytes() < master.size() || !disk.read(0, master)) {
        error = "Could not read the live 1024-byte APA master";
        return false;
    }
    if (is_hybrid_gpt_master(master)) {
        error = "Refusing bootstrap restore because the live master contains PC MBR/GPT evidence";
        return false;
    }
    if (!is_standard_apa_master(master)) {
        error = "Bootstrap restore requires a canonical live APA master; damaged masters belong to forensic recovery";
        return false;
    }
    return true;
}

[[nodiscard]] bool read_file_bounded(const std::filesystem::path& path,
                                     std::size_t max_bytes,
                                     std::vector<std::byte>& bytes,
                                     std::string& error)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "Could not inspect recovery artifact size: " + ec.message();
        return false;
    }
    if (size > max_bytes || size > std::numeric_limits<std::size_t>::max()) {
        error = "Recovery artifact exceeds the supported bounded size";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open recovery artifact";
        return false;
    }
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if ((!input && !bytes.empty()) || static_cast<std::size_t>(input.gcount()) != bytes.size()) {
        error = "Could not read the complete recovery artifact";
        return false;
    }
    return true;
}

[[nodiscard]] bool file_exists(const std::filesystem::path& path,
                               bool& exists,
                               std::string& error)
{
    std::error_code ec;
    exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "Could not inspect recovery artifact path: " + ec.message();
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_live_payload_bounds(
    const std::array<std::byte, kRescueApaHeaderBytes>& current,
    std::uint32_t start,
    std::uint32_t sectors,
    std::uint64_t device_bytes,
    std::string& error)
{
    if (start == 0 || sectors == 0) {
        error = "Bootstrap payload pointer is empty";
        return false;
    }
    if (sectors > kMaxMbrPayloadBytes / kSectorSize) {
        error = "Bootstrap payload exceeds the FHDB Manager 4 MiB safety limit";
        return false;
    }
    if (start < kMbrPayloadStart) {
        error = "Bootstrap payload starts before the reserved __mbr program area";
        return false;
    }

    const auto mbr_start = load_u32(current.data() + kApaStartOffset);
    const auto mbr_length = load_u32(current.data() + kApaLengthOffset);
    if (mbr_start != 0 || start >= mbr_length || sectors > mbr_length - start) {
        error = "Bootstrap payload does not fit the live __mbr geometry";
        return false;
    }

    const auto byte_offset = static_cast<std::uint64_t>(start) * kSectorSize;
    const auto byte_count = static_cast<std::uint64_t>(sectors) * kSectorSize;
    if (byte_offset > device_bytes || byte_count > device_bytes - byte_offset) {
        error = "Bootstrap payload extends outside the target device";
        return false;
    }
    return true;
}

[[nodiscard]] bool read_legacy_header(const std::filesystem::path& path,
                                      const std::array<std::byte, kRescueApaHeaderBytes>& current,
                                      std::array<std::byte, kRescueApaHeaderBytes>& saved,
                                      std::uint32_t& start,
                                      std::uint32_t& sectors)
{
    std::vector<std::byte> bytes;
    std::string ignored;
    if (!read_file_bounded(path, kRescueApaHeaderBytes, bytes, ignored) ||
        bytes.size() != kRescueApaHeaderBytes) {
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), saved.begin());
    if (!is_standard_apa_master(saved) || !same_disk_identity(current, saved)) {
        return false;
    }
    start = load_u32(saved.data() + kApaOsdStartOffset);
    sectors = load_u32(saved.data() + kApaOsdSizeOffset);
    return start != 0 && sectors != 0;
}

[[nodiscard]] bool publish_pointer(WritableBlockDevice& disk,
                                   const std::array<std::byte, kRescueApaHeaderBytes>& current,
                                   std::uint32_t start,
                                   std::uint32_t sectors,
                                   std::array<std::byte, kRescueApaHeaderBytes>& published,
                                   std::string& error)
{
    published = current;
    store_u32(published.data() + kApaOsdStartOffset, start);
    store_u32(published.data() + kApaOsdSizeOffset, sectors);
    store_u32(published.data() + kApaChecksumOffset, raw_apa_checksum(published));

    if (!disk.write(0, published)) {
        error = "Could not publish restored bootstrap pointer in the APA master";
        return false;
    }
    if (!disk.flush()) {
        error = "Could not durably flush the restored bootstrap pointer";
        return false;
    }
    std::array<std::byte, kRescueApaHeaderBytes> readback{};
    if (!disk.read(0, readback) || readback != published || !is_standard_apa_master(readback)) {
        error = "Restored bootstrap pointer failed exact APA-master read-back verification";
        return false;
    }
    return true;
}

[[nodiscard]] bool rollback_master(WritableBlockDevice& disk,
                                   const std::array<std::byte, kRescueApaHeaderBytes>& original)
{
    if (!disk.write(0, original) || !disk.flush()) {
        return false;
    }
    std::array<std::byte, kRescueApaHeaderBytes> readback{};
    return disk.read(0, readback) && readback == original;
}

} // namespace

BootstrapRestorePlan plan_bootstrap_restore(
    BlockDevice& disk,
    const std::filesystem::path& artifact_directory)
{
    BootstrapRestorePlan plan;
    if (!read_current_master(disk, plan.source_master, plan.error)) {
        return plan;
    }

    bool saw_rescue = false;
    bool saw_invalid_rescue = false;
    bool saw_header_only = false;
    std::string first_rescue_error;
    constexpr std::array<const char*, 2> rescue_names{"HDDRESCUE.BIN", "HDDRESCUE2.BIN"};

    for (const auto* name : rescue_names) {
        const auto path = artifact_directory / name;
        bool exists = false;
        std::string path_error;
        if (!file_exists(path, exists, path_error)) {
            plan.error = std::move(path_error);
            return plan;
        }
        if (!exists) {
            continue;
        }
        saw_rescue = true;

        std::vector<std::byte> bytes;
        std::string read_error;
        if (!read_file_bounded(path, kMaxRescueFileBytes, bytes, read_error)) {
            saw_invalid_rescue = true;
            if (first_rescue_error.empty()) first_rescue_error = std::move(read_error);
            continue;
        }
        const auto rescue = validate_rescue_image(bytes);
        if (!rescue.ok) {
            saw_invalid_rescue = true;
            if (first_rescue_error.empty()) first_rescue_error = rescue.error;
            continue;
        }
        if (!same_disk_identity(plan.source_master, rescue.apa_header)) {
            saw_invalid_rescue = true;
            if (first_rescue_error.empty()) first_rescue_error = "FHDB Rescue Capsule belongs to a different PS2 HDD";
            continue;
        }
        if ((rescue.info.flags & kRescueFlagHasPayload) == 0) {
            saw_header_only = true;
            continue;
        }
        if ((rescue.info.flags & kRescueFlagValidKelf) == 0) {
            saw_invalid_rescue = true;
            if (first_rescue_error.empty()) first_rescue_error = "FHDB Rescue Capsule payload is not a structurally validated KELF";
            continue;
        }
        if (!validate_live_payload_bounds(plan.source_master, rescue.info.payload_start,
                                          rescue.info.payload_sectors, disk.size_bytes(), plan.error)) {
            return plan;
        }

        plan.kind = BootstrapRestoreKind::rescue_payload;
        plan.source_path = path;
        plan.saved_master = rescue.apa_header;
        plan.payload = rescue.payload;
        plan.payload_start = rescue.info.payload_start;
        plan.payload_sectors = rescue.info.payload_sectors;
        plan.romver = rescue.info.romver;
        plan.family = rescue.info.family;
        plan.confidence = rescue.info.confidence;
        plan.ok = true;
        return plan;
    }

    // Match PS2 HDD Bootstrap Manager's fail-closed rule. If a full rescue slot
    // exists but is corrupt or foreign, silently falling back to an old pointer
    // backup would turn a warning sign into a write operation. Humans invented
    // enough exciting recovery stories already.
    if (saw_invalid_rescue) {
        plan.error = first_rescue_error.empty()
                         ? "An FHDB Rescue Capsule exists but is not safe to restore"
                         : first_rescue_error;
        return plan;
    }

    constexpr std::array<const char*, 4> legacy_names{
        "HDDMBR.BIN", "HDDMBR2.BIN", "FHDBMBR.BIN", "FHDBMBR2.BIN"};
    for (const auto* name : legacy_names) {
        const auto path = artifact_directory / name;
        bool exists = false;
        std::string path_error;
        if (!file_exists(path, exists, path_error)) {
            plan.error = std::move(path_error);
            return plan;
        }
        if (!exists) {
            continue;
        }
        std::array<std::byte, kRescueApaHeaderBytes> saved{};
        std::uint32_t start = 0;
        std::uint32_t sectors = 0;
        if (!read_legacy_header(path, plan.source_master, saved, start, sectors)) {
            continue;
        }
        if (!validate_live_payload_bounds(plan.source_master, start, sectors,
                                          disk.size_bytes(), plan.error)) {
            return plan;
        }
        plan.kind = BootstrapRestoreKind::legacy_pointer;
        plan.source_path = path;
        plan.saved_master = saved;
        plan.payload_start = start;
        plan.payload_sectors = sectors;
        plan.ok = true;
        return plan;
    }

    if (saw_header_only) {
        plan.error = "Only a header-only FHDB Rescue Capsule was found and no enabled HDDMBR/FHDBMBR pointer backup is available";
    } else if (saw_rescue) {
        plan.error = "No restorable FHDB bootstrap artifact was found";
    } else {
        plan.error = "No HDDRESCUE, HDDMBR, or legacy FHDBMBR restore artifact was found";
    }
    return plan;
}

BootstrapRestoreResult apply_bootstrap_restore(
    WritableBlockDevice& disk,
    const BootstrapRestorePlan& plan,
    const std::filesystem::path& safety_directory)
{
    BootstrapRestoreResult result;
    result.kind = plan.kind;
    result.source_path = plan.source_path;
    result.payload_start = plan.payload_start;
    result.payload_sectors = plan.payload_sectors;
    result.payload_bytes = plan.payload.size();

    if (!plan.ok || plan.kind == BootstrapRestoreKind::none) {
        result.error = plan.error.empty() ? "Bootstrap restore plan is not applicable" : plan.error;
        return result;
    }

    std::array<std::byte, kRescueApaHeaderBytes> current{};
    if (!read_current_master(disk, current, result.error)) {
        return result;
    }
    if (current != plan.source_master) {
        result.error = "Live APA master changed after restore preflight; refusing a stale recovery plan";
        return result;
    }
    if (!same_disk_identity(current, plan.saved_master)) {
        result.error = "Restore artifact no longer matches the live PS2 HDD identity";
        return result;
    }
    if (!validate_live_payload_bounds(current, plan.payload_start, plan.payload_sectors,
                                      disk.size_bytes(), result.error)) {
        return result;
    }
    if (plan.kind == BootstrapRestoreKind::rescue_payload &&
        plan.payload.size() != static_cast<std::uint64_t>(plan.payload_sectors) * kSectorSize) {
        result.error = "Frozen FHDB rescue payload no longer matches its sector geometry";
        return result;
    }

    const auto safety = save_hddmbr(safety_directory, current);
    if (!safety.ok) {
        result.error = "Mandatory current-master safety backup failed; no target write was performed: " + safety.error;
        return result;
    }
    result.safety_backup_path = safety.path;

    if (plan.kind == BootstrapRestoreKind::rescue_payload) {
        const auto payload_offset = static_cast<std::uint64_t>(plan.payload_start) * kSectorSize;
        if (!disk.write(payload_offset, plan.payload)) {
            result.error = "Could not write FHDB rescue payload into the reserved __mbr area";
            result.partial = true;
            return result;
        }
        if (!disk.flush()) {
            result.error = "Could not durably flush FHDB rescue payload before pointer publication";
            result.partial = true;
            return result;
        }
        std::vector<std::byte> readback(plan.payload.size());
        if (!disk.read(payload_offset, readback) || readback != plan.payload) {
            result.error = "FHDB rescue payload failed exact read-back verification; bootstrap pointer was not published";
            result.partial = true;
            return result;
        }
        result.payload_verified = true;
    }

    std::array<std::byte, kRescueApaHeaderBytes> published{};
    if (!publish_pointer(disk, current, plan.payload_start, plan.payload_sectors,
                         published, result.error)) {
        result.rollback_attempted = true;
        result.rollback_ok = rollback_master(disk, current);
        result.partial = plan.kind == BootstrapRestoreKind::rescue_payload;
        if (!result.rollback_ok) {
            result.warning = "Bootstrap pointer publication failed and exact master rollback also failed. Keep the mandatory HDDMBR safety backup.";
        } else if (result.partial) {
            result.warning = "APA master rollback succeeded. Verified rescue payload bytes may remain in the reserved __mbr program area, but the preflight pointer was restored.";
        }
        return result;
    }
    result.pointer_published = true;

    // A final normal APA parse catches accidental collateral damage to the
    // master. The rescue path is allowed to change a pointer, not reinvent APA.
    apa::Reader reader(disk);
    const auto scan = reader.scan();
    if (!scan.ok()) {
        result.error = "Bootstrap pointer was written but the normal APA parser rejected the resulting disk";
        result.rollback_attempted = true;
        result.rollback_ok = rollback_master(disk, current);
        result.pointer_published = !result.rollback_ok;
        result.partial = plan.kind == BootstrapRestoreKind::rescue_payload;
        if (!result.rollback_ok) {
            result.warning = "Final APA verification failed and exact master rollback failed. Keep the mandatory HDDMBR safety backup.";
        }
        return result;
    }
    result.final_apa_verified = true;
    result.ok = true;
    return result;
}

BootstrapRestoreResult restore_bootstrap_from_directory_to_image(
    WritableBlockDevice& disk,
    const std::filesystem::path& artifact_directory,
    const std::filesystem::path& safety_directory)
{
    const auto plan = plan_bootstrap_restore(disk, artifact_directory);
    if (!plan.ok) {
        BootstrapRestoreResult result;
        result.error = plan.error;
        return result;
    }
    return apply_bootstrap_restore(disk, plan, safety_directory);
}

} // namespace ps2hdd::fhdb
