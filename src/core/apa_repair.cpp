#include "ps2hdd/apa_repair.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace ps2hdd::repair {
namespace {

constexpr std::size_t kMagicOffset = 0x004;
constexpr std::size_t kIdOffset = 0x010;
constexpr std::size_t kStartOffset = 0x040;
constexpr std::size_t kTypeOffset = 0x048;
constexpr std::size_t kMbrMagicOffset = 0x100;
constexpr std::size_t kMbrVersionOffset = 0x120;
constexpr std::size_t kOsdStartOffset = 0x130;
constexpr std::size_t kOsdSizeOffset = 0x134;
constexpr std::size_t kPcMbrSignatureOffset = 0x1fe;

constexpr std::array<std::byte, 4> kCanonicalApaMagic{
    std::byte{'A'}, std::byte{'P'}, std::byte{'A'}, std::byte{0}};
constexpr char kCanonicalMbrId[] = "__mbr";
constexpr char kCanonicalSonyMagic[] = "Sony Computer Entertainment Inc.";
constexpr std::uint16_t kMasterType = 0x0001;
constexpr std::uint32_t kMasterVersion = 2;

[[nodiscard]] std::uint16_t load_u16(const std::byte* p) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[0])) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(std::to_integer<unsigned char>(p[1])) << 8U);
}

[[nodiscard]] std::uint32_t load_u32(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

void store_u16(std::byte* p, std::uint16_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void store_u32(std::byte* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::byte>(value & 0xffU);
    p[1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    p[2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    p[3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::uint32_t checksum(std::span<const std::byte, kApaHeaderBytes> header) noexcept
{
    std::uint32_t sum = 0;
    for (std::size_t word = 1; word < 256; ++word) {
        sum += load_u32(header.data() + static_cast<std::ptrdiff_t>(word * 4));
    }
    return sum;
}

[[nodiscard]] bool hybrid_gpt(std::span<const std::byte, kApaHeaderBytes> header) noexcept
{
    return header[kPcMbrSignatureOffset] == std::byte{0x55} &&
           header[kPcMbrSignatureOffset + 1] == std::byte{0xaa};
}

void apply_known_fixes(std::span<std::byte, kApaHeaderBytes> header,
                       std::uint32_t fixes) noexcept
{
    if ((fixes & issue_apa_magic) != 0) {
        std::copy(kCanonicalApaMagic.begin(), kCanonicalApaMagic.end(),
                  header.begin() + kMagicOffset);
    }
    if ((fixes & issue_mbr_id) != 0) {
        std::memcpy(header.data() + kIdOffset, kCanonicalMbrId, sizeof(kCanonicalMbrId) - 1);
    }
    if ((fixes & issue_sony_magic) != 0) {
        std::memcpy(header.data() + kMbrMagicOffset, kCanonicalSonyMagic,
                    sizeof(kCanonicalSonyMagic) - 1);
    }
    if ((fixes & issue_master_start) != 0) {
        store_u32(header.data() + kStartOffset, 0);
    }
    if ((fixes & issue_master_type) != 0) {
        store_u16(header.data() + kTypeOffset, kMasterType);
    }
    if ((fixes & issue_mbr_version) != 0) {
        store_u32(header.data() + kMbrVersionOffset, kMasterVersion);
    }
}

} // namespace

ApaRepairPlan analyze_apa_master(
    std::span<const std::byte, kApaHeaderBytes> header) noexcept
{
    ApaRepairPlan plan;
    std::uint32_t identity_issues = 0;
    std::uint32_t anchor_issues = 0;

    if (!std::equal(kCanonicalApaMagic.begin(), kCanonicalApaMagic.end(),
                    header.begin() + kMagicOffset)) {
        identity_issues |= issue_apa_magic;
    } else {
        ++plan.identity_matches;
    }
    if (std::memcmp(header.data() + kIdOffset, kCanonicalMbrId,
                    sizeof(kCanonicalMbrId) - 1) != 0) {
        identity_issues |= issue_mbr_id;
    } else {
        ++plan.identity_matches;
    }
    if (std::memcmp(header.data() + kMbrMagicOffset, kCanonicalSonyMagic,
                    sizeof(kCanonicalSonyMagic) - 1) != 0) {
        identity_issues |= issue_sony_magic;
    } else {
        ++plan.identity_matches;
    }

    if (load_u32(header.data() + kStartOffset) != 0) {
        anchor_issues |= issue_master_start;
    } else {
        ++plan.master_anchor_matches;
    }
    if (load_u16(header.data() + kTypeOffset) != kMasterType) {
        anchor_issues |= issue_master_type;
    } else {
        ++plan.master_anchor_matches;
    }
    if (load_u32(header.data() + kMbrVersionOffset) != kMasterVersion) {
        anchor_issues |= issue_mbr_version;
    } else {
        ++plan.master_anchor_matches;
    }

    plan.issues = identity_issues | anchor_issues;
    const auto stored_checksum = load_u32(header.data());
    const auto calculated_checksum = checksum(header);
    if (stored_checksum != calculated_checksum) {
        plan.issues |= issue_checksum;
    }

    const auto osd_start = load_u32(header.data() + kOsdStartOffset);
    const auto osd_size = load_u32(header.data() + kOsdSizeOffset);
    if ((osd_start == 0) != (osd_size == 0)) {
        plan.issues |= issue_pointer_inconsistent;
        plan.pointer_clear_recommended = true;
    }

    if (hybrid_gpt(header)) {
        plan.blockers |= blocker_hybrid_gpt;
        return plan;
    }

    if (identity_issues != 0) {
        if (plan.identity_matches >= 2 && plan.master_anchor_matches == 3 &&
            std::popcount(identity_issues) == 1) {
            plan.safe_header_fixes |= identity_issues;
        } else {
            plan.blockers |= blocker_low_identity;
        }
    }

    if (anchor_issues != 0) {
        if (plan.identity_matches == 3 && plan.master_anchor_matches >= 2 &&
            std::popcount(anchor_issues) == 1) {
            plan.safe_header_fixes |= anchor_issues;
        } else {
            plan.blockers |= blocker_not_master;
        }
    }

    if (plan.safe_header_fixes != 0 && plan.blockers == 0) {
        if (stored_checksum == calculated_checksum) {
            plan.blockers |= blocker_unexplained_checksum;
        } else {
            std::array<std::byte, kApaHeaderBytes> candidate{};
            std::copy(header.begin(), header.end(), candidate.begin());
            apply_known_fixes(candidate, plan.safe_header_fixes);
            if (checksum(candidate) != stored_checksum) {
                plan.blockers |= blocker_unexplained_checksum;
            }
        }
    } else if (stored_checksum != calculated_checksum) {
        plan.blockers |= blocker_unexplained_checksum;
    }

    plan.header_patch_safe = plan.blockers == 0 && plan.safe_header_fixes != 0;
    return plan;
}

bool build_repaired_apa_master(
    std::span<const std::byte, kApaHeaderBytes> source,
    const ApaRepairPlan& plan,
    std::array<std::byte, kApaHeaderBytes>& repaired,
    std::string& error) noexcept
{
    error.clear();
    if (!plan.header_patch_safe || plan.blockers != 0 || plan.safe_header_fixes == 0) {
        error = "APA master repair plan is blocked or ambiguous";
        return false;
    }
    std::copy(source.begin(), source.end(), repaired.begin());
    apply_known_fixes(repaired, plan.safe_header_fixes);
    store_u32(repaired.data(), checksum(repaired));
    return true;
}

} // namespace ps2hdd::repair
