#include "ps2hdd/ps2_iso.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd::iso {
namespace {

[[nodiscard]] std::uint32_t load_u32_le(const std::byte* p) noexcept
{
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3])) << 24U);
}

[[nodiscard]] bool range_fits(std::uint64_t offset, std::uint64_t bytes,
                              std::uint64_t size) noexcept
{
    return offset <= size && bytes <= size - offset;
}

[[nodiscard]] char ascii_upper(char c) noexcept
{
    const auto value = static_cast<unsigned char>(c);
    return static_cast<char>(std::toupper(value));
}

[[nodiscard]] std::string upper_ascii(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), ascii_upper);
    return out;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool identifier_is_system_cnf(std::string_view identifier)
{
    const auto semicolon = identifier.find(';');
    if (semicolon != std::string_view::npos) {
        identifier = identifier.substr(0, semicolon);
    }
    return upper_ascii(identifier) == "SYSTEM.CNF";
}

[[nodiscard]] std::string basename_from_boot(std::string_view value)
{
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
        value = trim(value);
    }

    const auto slash = value.find_last_of("\\/");
    if (slash != std::string_view::npos) {
        value.remove_prefix(slash + 1);
    }
    const auto colon = value.find_last_of(':');
    if (colon != std::string_view::npos) {
        value.remove_prefix(colon + 1);
    }
    const auto semicolon = value.find(';');
    if (semicolon != std::string_view::npos) {
        value = value.substr(0, semicolon);
    }
    value = trim(value);
    return upper_ascii(value);
}

[[nodiscard]] bool plausible_startup(std::string_view startup) noexcept
{
    // Common PS2 disc IDs are twelve characters such as SLUS_123.45, but some
    // software uses a dash in the prefix separator. Be strict about the shape
    // that HDL metadata can store while accepting both separators.
    if (startup.size() != 11) {
        return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (startup[i] < 'A' || startup[i] > 'Z') {
            return false;
        }
    }
    if (startup[4] != '_' && startup[4] != '-') {
        return false;
    }
    for (std::size_t i = 5; i <= 7; ++i) {
        if (startup[i] < '0' || startup[i] > '9') {
            return false;
        }
    }
    if (startup[8] != '.') {
        return false;
    }
    return startup[9] >= '0' && startup[9] <= '9' &&
           startup[10] >= '0' && startup[10] <= '9';
}

[[nodiscard]] std::string parse_startup(std::string_view system_cnf)
{
    std::size_t start = 0;
    while (start <= system_cnf.size()) {
        const auto end = system_cnf.find_first_of("\r\n", start);
        const auto line = trim(system_cnf.substr(
            start, end == std::string_view::npos ? system_cnf.size() - start : end - start));
        const auto equals = line.find('=');
        if (equals != std::string_view::npos) {
            const auto key = upper_ascii(trim(line.substr(0, equals)));
            if (key == "BOOT2" || key == "BOOT") {
                const auto startup = basename_from_boot(line.substr(equals + 1));
                if (plausible_startup(startup)) {
                    return startup;
                }
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
        while (start < system_cnf.size() &&
               (system_cnf[start] == '\r' || system_cnf[start] == '\n')) {
            ++start;
        }
    }
    return {};
}

} // namespace

GameSourceResult inspect_ps2_iso(BlockDevice& device)
{
    GameSourceResult result;
    result.game.image_bytes = device.size_bytes();

    constexpr std::uint64_t pvd_offset =
        static_cast<std::uint64_t>(kPrimaryVolumeDescriptorLba) * kSectorSize;
    if (!range_fits(pvd_offset, kSectorSize, device.size_bytes())) {
        result.error = "Image is too small to contain an ISO9660 primary volume descriptor";
        return result;
    }

    std::array<std::byte, kSectorSize> pvd{};
    if (!device.read(pvd_offset, pvd)) {
        result.error = "Could not read ISO9660 primary volume descriptor";
        return result;
    }
    if (std::to_integer<unsigned char>(pvd[0]) != 1 ||
        std::string_view(reinterpret_cast<const char*>(pvd.data() + 1), 5) != "CD001" ||
        std::to_integer<unsigned char>(pvd[6]) != 1) {
        result.error = "ISO9660 primary volume descriptor is missing or unsupported";
        return result;
    }

    constexpr std::size_t root_record = 156;
    const auto root_length = std::to_integer<unsigned char>(pvd[root_record]);
    if (root_length < 34 || root_record + root_length > pvd.size()) {
        result.error = "ISO9660 root directory record is invalid";
        return result;
    }
    const auto root_lba = load_u32_le(pvd.data() + root_record + 2);
    const auto root_bytes = load_u32_le(pvd.data() + root_record + 10);
    if (root_bytes == 0 || root_bytes > 16U * 1024U * 1024U) {
        result.error = "ISO9660 root directory size is invalid or implausibly large";
        return result;
    }

    const std::uint64_t directory_offset = static_cast<std::uint64_t>(root_lba) * kSectorSize;
    if (!range_fits(directory_offset, root_bytes, device.size_bytes())) {
        result.error = "ISO9660 root directory extends outside the image";
        return result;
    }

    std::vector<std::byte> directory(root_bytes);
    if (!device.read(directory_offset, directory)) {
        result.error = "Could not read ISO9660 root directory";
        return result;
    }

    std::uint32_t system_lba = 0;
    std::uint32_t system_bytes = 0;
    std::size_t position = 0;
    while (position < directory.size()) {
        const auto record_length = std::to_integer<unsigned char>(directory[position]);
        if (record_length == 0) {
            const auto next_sector = ((position / kSectorSize) + 1) * kSectorSize;
            if (next_sector <= position) {
                break;
            }
            position = next_sector;
            continue;
        }
        if (record_length < 34 || record_length > directory.size() - position) {
            result.error = "ISO9660 root directory contains a malformed record";
            return result;
        }
        const auto name_length = std::to_integer<unsigned char>(directory[position + 32]);
        if (33U + name_length > record_length) {
            result.error = "ISO9660 directory record filename exceeds its record";
            return result;
        }
        const auto flags = std::to_integer<unsigned char>(directory[position + 25]);
        const std::string_view identifier(
            reinterpret_cast<const char*>(directory.data() + position + 33), name_length);
        if ((flags & 0x02U) == 0 && identifier_is_system_cnf(identifier)) {
            system_lba = load_u32_le(directory.data() + position + 2);
            system_bytes = load_u32_le(directory.data() + position + 10);
            break;
        }
        position += record_length;
    }

    if (system_bytes == 0) {
        result.error = "SYSTEM.CNF was not found in the ISO9660 root directory";
        return result;
    }
    if (system_bytes > 1024U * 1024U) {
        result.error = "SYSTEM.CNF is implausibly large";
        return result;
    }

    const std::uint64_t system_offset = static_cast<std::uint64_t>(system_lba) * kSectorSize;
    if (!range_fits(system_offset, system_bytes, device.size_bytes())) {
        result.error = "SYSTEM.CNF extends outside the ISO image";
        return result;
    }

    std::vector<std::byte> system(system_bytes);
    if (!device.read(system_offset, system)) {
        result.error = "Could not read SYSTEM.CNF";
        return result;
    }
    result.game.system_cnf.assign(reinterpret_cast<const char*>(system.data()), system.size());
    result.game.startup = parse_startup(result.game.system_cnf);
    if (result.game.startup.empty()) {
        result.error = "SYSTEM.CNF does not contain a supported PS2 BOOT/BOOT2 startup ID";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::iso
