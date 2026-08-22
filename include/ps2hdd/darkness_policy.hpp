#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ps2hdd::darkness_policy {

// Startup discovery may auto-open only the one unambiguous PS2 candidate, and
// never replaces a source the user already opened.
[[nodiscard]] constexpr std::optional<std::size_t>
auto_open_candidate(std::size_t candidate_count, bool source_already_open) noexcept
{
    if (!source_already_open && candidate_count == 1U) {
        return std::size_t{0};
    }
    return std::nullopt;
}

// Pure form of the Windows drive-letter policy. Bit 0 is A:, bit 1 is B:, etc.
// A: and B: are never selected; DriveForge behaves like a newly attached data
// volume and takes the lowest currently unused letter from C: through Z:.
[[nodiscard]] constexpr std::optional<wchar_t>
choose_mount_letter(std::uint32_t occupied_mask) noexcept
{
    for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
        const auto bit = std::uint32_t{1} << static_cast<unsigned>(letter - L'A');
        if ((occupied_mask & bit) == 0U) {
            return letter;
        }
    }
    return std::nullopt;
}

} // namespace ps2hdd::darkness_policy
