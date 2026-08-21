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

// Pure form of the Darkness drive-letter policy. Bit 0 is A:, bit 1 is B:,
// etc. Windows supplies the occupied mask through GetLogicalDrives().
[[nodiscard]] constexpr std::optional<wchar_t>
choose_mount_letter(std::uint32_t occupied_mask, wchar_t preferred = L'P') noexcept
{
    auto upper_ascii = [](wchar_t letter) constexpr {
        return (letter >= L'a' && letter <= L'z')
                   ? static_cast<wchar_t>(letter - (L'a' - L'A'))
                   : letter;
    };
    auto available = [occupied_mask](wchar_t letter) constexpr {
        if (letter < L'D' || letter > L'Z') {
            return false;
        }
        const auto bit = std::uint32_t{1} << static_cast<unsigned>(letter - L'A');
        return (occupied_mask & bit) == 0U;
    };

    preferred = upper_ascii(preferred);
    if (available(preferred)) {
        return preferred;
    }
    for (int value = static_cast<int>(L'P'); value <= static_cast<int>(L'Z'); ++value) {
        const auto letter = static_cast<wchar_t>(value);
        if (letter != preferred && available(letter)) {
            return letter;
        }
    }
    for (int value = static_cast<int>(L'O'); value >= static_cast<int>(L'D'); --value) {
        const auto letter = static_cast<wchar_t>(value);
        if (letter != preferred && available(letter)) {
            return letter;
        }
    }
    return std::nullopt;
}

} // namespace ps2hdd::darkness_policy
