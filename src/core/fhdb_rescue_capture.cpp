#include "ps2hdd/fhdb_rescue_capture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace ps2hdd::fhdb {
namespace {

constexpr std::size_t kApaStartOffset = 0x040;
constexpr std::size_t kApaLengthOffset = 0x044;
constexpr std::size_t kApaOsdStartOffset = 0x130;
constexpr std::size_t kApaOsdSizeOffset = 0x134;
constexpr std::uint64_t kSectorBytes = 512ULL;

[[nodiscard]] std::uint32_t load_u32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

[[nodiscard]] bool range_fits(std::uint64_t offset,
                              std::uint64_t bytes,
                              std::uint64_t device_bytes) noexcept
{
    return offset <= device_bytes && bytes <= device_bytes - offset;
}

} // namespace

RescueImageResult capture_rescue_image(BlockDevice& disk,
                                       const RescueCaptureOptions& options)
{
    RescueImageResult result;
    if (disk.size_bytes() < kRescueApaHeaderBytes) {
        result.error = "Cannot capture FHDB Rescue Capsule from a device smaller than the APA master";
        return result;
    }

    std::array<std::byte, kRescueApaHeaderBytes> master{};
    if (!disk.read(0, master)) {
        result.error = "Could not read the live 1024-byte APA master for Rescue Capsule capture";
        return result;
    }

    // Check PC ownership before interpreting osdStart/osdSize. A perfectly
    // parseable PS2-looking header embedded in a dual-owned disk is not evidence
    // that DriveForge owns sector zero. Recovery artifacts must never bless a
    // layout that the normal write gate would refuse later.
    if (is_hybrid_gpt_master(master)) {
        result.error = "Refusing Rescue Capsule capture because the live master contains PC MBR/GPT evidence";
        return result;
    }
    if (!is_standard_apa_master(master)) {
        result.error = "Rescue Capsule capture requires a canonical live APA __mbr master";
        return result;
    }

    const auto mbr_start = load_u32(master.data() + kApaStartOffset);
    const auto mbr_length = load_u32(master.data() + kApaLengthOffset);
    const auto payload_start = load_u32(master.data() + kApaOsdStartOffset);
    const auto payload_sectors = load_u32(master.data() + kApaOsdSizeOffset);

    if (mbr_start != 0 || mbr_length == 0) {
        result.error = "Live APA __mbr geometry is invalid for Rescue Capsule capture";
        return result;
    }

    if (payload_start == 0 && payload_sectors == 0) {
        // Header-only capsules are meaningful. They prove which disk/master was
        // observed while recording that no active HDD bootstrap was published.
        return build_rescue_image(master, {}, 0, 0,
                                  options.romver, options.family, options.confidence);
    }
    if (payload_start == 0 || payload_sectors == 0) {
        result.error = "Live APA bootstrap pointer is half-empty; refusing to invent Rescue Capsule geometry";
        return result;
    }

    constexpr std::uint32_t max_sectors =
        kBootstrapPayloadMaxBytes / static_cast<std::uint32_t>(kSectorBytes);
    if (payload_sectors > max_sectors) {
        result.error = "Live bootstrap payload exceeds the FHDB 4 MiB Rescue Capsule safety limit";
        return result;
    }
    if (payload_start < kBootstrapProgramStartSector) {
        result.error = "Live bootstrap payload starts before the reserved __mbr program area";
        return result;
    }
    if (payload_start >= mbr_length || payload_sectors > mbr_length - payload_start) {
        result.error = "Live bootstrap payload does not fit the __mbr partition geometry";
        return result;
    }

    const auto payload_offset = static_cast<std::uint64_t>(payload_start) * kSectorBytes;
    const auto payload_bytes = static_cast<std::uint64_t>(payload_sectors) * kSectorBytes;
    if (!range_fits(payload_offset, payload_bytes, disk.size_bytes()) ||
        payload_bytes > std::numeric_limits<std::size_t>::max()) {
        result.error = "Live bootstrap payload extends outside the backing device";
        return result;
    }

    std::vector<std::byte> payload(static_cast<std::size_t>(payload_bytes));
    if (!disk.read(payload_offset, payload)) {
        result.error = "Could not read the complete live bootstrap payload for Rescue Capsule capture";
        return result;
    }

    // build_rescue_image owns the shared wire-format policy: hashes are derived
    // from the exact captured bytes and VALID_KELF is earned by structural KELF
    // validation rather than by the caller declaring what it hoped to capture.
    return build_rescue_image(master, payload, payload_start, payload_sectors,
                              options.romver, options.family, options.confidence);
}

} // namespace ps2hdd::fhdb
