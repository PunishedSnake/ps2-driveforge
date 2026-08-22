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

    expect(choose_mount_letter(0) == std::optional<wchar_t>{L'C'},
           "C: should be selected when it is the first free data letter");
    expect(choose_mount_letter(bit(L'C')) == std::optional<wchar_t>{L'D'},
           "D: should be selected when C: is occupied");
    expect(choose_mount_letter(bit(L'C') | bit(L'D')) == std::optional<wchar_t>{L'E'},
           "selection should advance monotonically to the next free letter");

    // A: and B: are legacy floppy letters and are never DriveForge mount targets,
    // regardless of whether Windows reports them as occupied.
    expect(choose_mount_letter(bit(L'A') | bit(L'B')) == std::optional<wchar_t>{L'C'},
           "A:/B: occupancy must not affect the first eligible C: mount letter");

    std::uint32_t through_y = 0;
    for (wchar_t letter = L'C'; letter <= L'Y'; ++letter) {
        through_y |= bit(letter);
    }
    expect(choose_mount_letter(through_y) == std::optional<wchar_t>{L'Z'},
           "Z: should be selected when it is the last free data letter");

    std::uint32_t all_data_letters = 0;
    for (wchar_t letter = L'C'; letter <= L'Z'; ++letter) {
        all_data_letters |= bit(letter);
    }
    expect(!choose_mount_letter(all_data_letters).has_value(),
           "no mount letter should be returned when C:-Z: are all occupied");

    if (failures != 0) {
        std::cerr << failures << " Darkness policy test(s) failed\n";
        return 1;
    }

    std::cout << "Darkness GUI/mount policy tests passed\n";
    return 0;
}
