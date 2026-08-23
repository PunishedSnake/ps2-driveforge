#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace ps2hdd::repair {

inline constexpr std::size_t kApaHeaderBytes = 1024;

enum ApaRepairIssue : std::uint32_t {
    issue_checksum = 1U << 0,
    issue_apa_magic = 1U << 1,
    issue_mbr_id = 1U << 2,
    issue_sony_magic = 1U << 3,
    issue_master_start = 1U << 4,
    issue_master_type = 1U << 5,
    issue_mbr_version = 1U << 6,
    issue_pointer_inconsistent = 1U << 7,
};

enum ApaRepairBlocker : std::uint32_t {
    blocker_hybrid_gpt = 1U << 0,
    blocker_low_identity = 1U << 1,
    blocker_not_master = 1U << 2,
    blocker_unexplained_checksum = 1U << 3,
};

struct ApaRepairPlan {
    std::uint32_t issues{};
    std::uint32_t safe_header_fixes{};
    std::uint32_t blockers{};
    unsigned identity_matches{};
    unsigned master_anchor_matches{};
    bool header_patch_safe{};
    bool pointer_clear_recommended{};
};

// Byte-for-byte policy port of fhdb-bootstrap-manager apa_repair. It examines
// only canonical master identity/anchor fields and never guesses chain links,
// length, passwords, timestamps, sub-partitions or filesystem contents.
[[nodiscard]] ApaRepairPlan analyze_apa_master(
    std::span<const std::byte, kApaHeaderBytes> header) noexcept;

// Materialize only plan.safe_header_fixes plus the checksum word. Blocked or
// ambiguous plans are refused.
[[nodiscard]] bool build_repaired_apa_master(
    std::span<const std::byte, kApaHeaderBytes> source,
    const ApaRepairPlan& plan,
    std::array<std::byte, kApaHeaderBytes>& repaired,
    std::string& error) noexcept;

} // namespace ps2hdd::repair
