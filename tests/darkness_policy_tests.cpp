#include "../include/ps2hdd/darkness_policy.hpp"

#include <cstdint>
#include <iostream>
#include <optional>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::uint32_t bit(wchar_t letter)
{
    return std::uint32_t{1} << static_cast<unsigned>(letter - L'A');
}

} // namespace

int main()
{
    using ps2hdd::darkness_policy::auto_open_candidate;
    using ps2hdd::darkness_policy::choose_mount_letter;

    expect(!auto_open_candidate(0, false).has_value(),
           "zero PS2 candidates must not auto-open");
    expect(auto_open_candidate(1, false) == std::optional<std::size_t>{0},
           "exactly one PS2 candidate should auto-open when no source is open");
    expect(!auto_open_candidate(2, false).has_value(),
           "multiple PS2 candidates require an explicit user choice");
    expect(!auto_open_candidate(1, true).has_value(),
           "discovery must not replace an already-open source");

    expect(choose_mount_letter(0) == std::optional<wchar_t>{L'P'},
           "P: should be preferred when free");
    expect(choose_mount_letter(bit(L'P')) == std::optional<wchar_t>{L'Q'},
           "Q: should follow when P: is occupied");
    expect(choose_mount_letter(0, L'r') == std::optional<wchar_t>{L'R'},
           "preferred drive letter should be ASCII case-insensitive");

    std::uint32_t p_through_z = 0;
    for (wchar_t letter = L'P'; letter <= L'Z'; ++letter) {
        p_through_z |= bit(letter);
    }
    expect(choose_mount_letter(p_through_z) == std::optional<wchar_t>{L'O'},
           "fallback should walk backward from O: after P:-Z: are occupied");

    std::uint32_t all_data_letters = 0;
    for (wchar_t letter = L'D'; letter <= L'Z'; ++letter) {
        all_data_letters |= bit(letter);
    }
    expect(!choose_mount_letter(all_data_letters).has_value(),
           "no mount letter should be returned when D:-Z: are all occupied");

    expect(choose_mount_letter(0, L'C') == std::optional<wchar_t>{L'P'},
           "invalid preferred system letter must fall back to P:");

    if (failures != 0) {
        std::cerr << failures << " Darkness policy test(s) failed\n";
        return 1;
    }

    std::cout << "Darkness GUI/mount policy tests passed\n";
    return 0;
}
