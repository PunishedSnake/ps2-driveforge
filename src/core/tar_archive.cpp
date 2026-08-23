#include "ps2hdd/tar_archive.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd::tar {
namespace {

constexpr std::size_t kRecordSize = 512;
constexpr std::size_t kNameOffset = 0;
constexpr std::size_t kNameSize = 100;
constexpr std::size_t kModeOffset = 100;
constexpr std::size_t kUidOffset = 108;
constexpr std::size_t kGidOffset = 116;
constexpr std::size_t kSizeOffset = 124;
constexpr std::size_t kSizeSize = 12;
constexpr std::size_t kMtimeOffset = 136;
constexpr std::size_t kChecksumOffset = 148;
constexpr std::size_t kChecksumSize = 8;
constexpr std::size_t kTypeOffset = 156;
constexpr std::size_t kMagicOffset = 257;
constexpr std::size_t kVersionOffset = 263;
constexpr std::size_t kPrefixOffset = 345;
constexpr std::size_t kPrefixSize = 155;

bool zero_record(std::span<const std::byte> record) noexcept
{
    return std::all_of(record.begin(), record.end(), [](std::byte value) {
        return value == std::byte{0};
    });
}

std::string field_string(std::span<const std::byte> field)
{
    std::size_t length = 0;
    while (length < field.size() && field[length] != std::byte{0}) {
        ++length;
    }
    return std::string(reinterpret_cast<const char*>(field.data()), length);
}

bool safe_name(std::string_view name) noexcept
{
    if (name.empty() || name.front() == '/' || name.front() == '\\') {
        return false;
    }
    std::size_t cursor = 0;
    while (cursor <= name.size()) {
        const auto slash = name.find_first_of("/\\", cursor);
        const auto end = slash == std::string_view::npos ? name.size() : slash;
        const auto component = name.substr(cursor, end - cursor);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        cursor = slash + 1U;
    }
    return true;
}

bool parse_octal(std::span<const std::byte> field, std::uint64_t& value) noexcept
{
    value = 0;
    std::size_t cursor = 0;
    while (cursor < field.size() &&
           (field[cursor] == std::byte{' '} || field[cursor] == std::byte{0})) {
        ++cursor;
    }
    bool saw_digit = false;
    for (; cursor < field.size(); ++cursor) {
        const auto raw = std::to_integer<unsigned char>(field[cursor]);
        if (raw == 0 || raw == static_cast<unsigned char>(' ')) {
            break;
        }
        if (raw < static_cast<unsigned char>('0') || raw > static_cast<unsigned char>('7')) {
            return false;
        }
        saw_digit = true;
        const auto digit = static_cast<std::uint64_t>(raw - static_cast<unsigned char>('0'));
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 8U) {
            return false;
        }
        value = value * 8U + digit;
    }
    return saw_digit || value == 0;
}

std::uint64_t header_checksum(std::span<const std::byte> header) noexcept
{
    std::uint64_t sum = 0;
    for (std::size_t index = 0; index < header.size(); ++index) {
        if (index >= kChecksumOffset && index < kChecksumOffset + kChecksumSize) {
            sum += static_cast<unsigned char>(' ');
        } else {
            sum += std::to_integer<unsigned char>(header[index]);
        }
    }
    return sum;
}

bool write_octal(std::span<std::byte> field, std::uint64_t value) noexcept
{
    if (field.size() < 2) {
        return false;
    }
    std::fill(field.begin(), field.end(), std::byte{'0'});
    field.back() = std::byte{0};
    std::size_t cursor = field.size() - 1U;
    while (value != 0) {
        if (cursor == 0) {
            return false;
        }
        --cursor;
        field[cursor] = static_cast<std::byte>('0' + (value & 7U));
        value >>= 3U;
    }
    return true;
}

bool write_checksum(std::span<std::byte> header) noexcept
{
    std::fill(header.begin() + static_cast<std::ptrdiff_t>(kChecksumOffset),
              header.begin() + static_cast<std::ptrdiff_t>(kChecksumOffset + kChecksumSize),
              std::byte{' '});
    const auto sum = header_checksum(header);
    auto field = header.subspan(kChecksumOffset, kChecksumSize);
    std::fill(field.begin(), field.end(), std::byte{'0'});
    field[6] = std::byte{0};
    field[7] = std::byte{' '};
    std::uint64_t value = sum;
    for (std::size_t pos = 6; pos-- > 0;) {
        field[pos] = static_cast<std::byte>('0' + (value & 7U));
        value >>= 3U;
    }
    return value == 0;
}

bool split_name(std::string_view name, std::string_view& prefix,
                std::string_view& leaf) noexcept
{
    if (name.size() <= kNameSize) {
        prefix = {};
        leaf = name;
        return true;
    }
    auto slash = name.rfind('/');
    while (slash != std::string_view::npos) {
        const auto candidate_prefix = name.substr(0, slash);
        const auto candidate_leaf = name.substr(slash + 1U);
        if (!candidate_prefix.empty() && candidate_prefix.size() <= kPrefixSize &&
            !candidate_leaf.empty() && candidate_leaf.size() <= kNameSize) {
            prefix = candidate_prefix;
            leaf = candidate_leaf;
            return true;
        }
        if (slash == 0) {
            break;
        }
        slash = name.rfind('/', slash - 1U);
    }
    return false;
}

bool set_member_name(std::span<std::byte> header, std::string_view name) noexcept
{
    std::string_view prefix;
    std::string_view leaf;
    if (!split_name(name, prefix, leaf)) {
        return false;
    }
    std::fill(header.begin() + static_cast<std::ptrdiff_t>(kNameOffset),
              header.begin() + static_cast<std::ptrdiff_t>(kNameOffset + kNameSize),
              std::byte{0});
    std::memcpy(header.data() + kNameOffset, leaf.data(), leaf.size());
    std::fill(header.begin() + static_cast<std::ptrdiff_t>(kPrefixOffset),
              header.begin() + static_cast<std::ptrdiff_t>(kPrefixOffset + kPrefixSize),
              std::byte{0});
    if (!prefix.empty()) {
        std::memcpy(header.data() + kPrefixOffset, prefix.data(), prefix.size());
    }
    return true;
}

std::string member_name(std::span<const std::byte> header)
{
    const auto name = field_string(header.subspan(kNameOffset, kNameSize));
    const auto prefix = field_string(header.subspan(kPrefixOffset, kPrefixSize));
    if (prefix.empty()) {
        return name;
    }
    return prefix + "/" + name;
}

bool append_bounded(std::vector<std::byte>& output, std::span<const std::byte> bytes,
                    std::uint64_t limit) noexcept
{
    if (bytes.size() > limit || output.size() > limit - bytes.size()) {
        return false;
    }
    output.insert(output.end(), bytes.begin(), bytes.end());
    return true;
}

bool append_payload(std::vector<std::byte>& output, std::span<const std::byte> bytes,
                    std::uint64_t limit) noexcept
{
    if (!append_bounded(output, bytes, limit)) {
        return false;
    }
    const auto padding = (kRecordSize - (bytes.size() % kRecordSize)) % kRecordSize;
    if (padding != 0) {
        std::array<std::byte, kRecordSize> zeros{};
        if (!append_bounded(output, std::span<const std::byte>(zeros).first(padding), limit)) {
            return false;
        }
    }
    return true;
}

std::array<std::byte, kRecordSize> new_header(const MemberPatch& patch, bool& ok)
{
    std::array<std::byte, kRecordSize> header{};
    ok = false;
    if (!set_member_name(header, patch.name)) {
        return header;
    }
    if (!write_octal(std::span<std::byte>(header).subspan(kModeOffset, 8), 0644) ||
        !write_octal(std::span<std::byte>(header).subspan(kUidOffset, 8), 0) ||
        !write_octal(std::span<std::byte>(header).subspan(kGidOffset, 8), 0) ||
        !write_octal(std::span<std::byte>(header).subspan(kSizeOffset, kSizeSize), patch.bytes.size()) ||
        !write_octal(std::span<std::byte>(header).subspan(kMtimeOffset, 12), 0)) {
        return header;
    }
    header[kTypeOffset] = std::byte{'0'};
    constexpr char magic[] = "ustar";
    std::memcpy(header.data() + kMagicOffset, magic, 5);
    header[kMagicOffset + 5] = std::byte{0};
    header[kVersionOffset] = std::byte{'0'};
    header[kVersionOffset + 1] = std::byte{'0'};
    ok = write_checksum(header);
    return header;
}

struct ParsedMember {
    std::string name;
    std::size_t header_offset{};
    std::size_t data_offset{};
    std::size_t payload_size{};
    std::size_t padded_size{};
    char type{};
};

} // namespace

UpdateResult update_archive(std::span<const std::byte> existing,
                            std::span<const MemberPatch> patches,
                            const UpdateOptions& options)
{
    UpdateResult result;
    if (options.max_archive_bytes < 2U * kRecordSize) {
        result.error = "TAR output limit is smaller than the two-record terminator";
        return result;
    }
    if (!existing.empty() && existing.size() % kRecordSize != 0) {
        result.error = "Existing TAR size is not a multiple of 512 bytes";
        return result;
    }
    if (existing.size() > options.max_archive_bytes) {
        result.error = "Existing TAR exceeds the configured archive byte limit";
        return result;
    }

    std::map<std::string, const MemberPatch*, std::less<>> patch_map;
    for (const auto& patch : patches) {
        if (!safe_name(patch.name)) {
            result.error = "TAR patch contains an unsafe or empty member name: " + patch.name;
            return result;
        }
        if (!patch_map.emplace(patch.name, &patch).second) {
            result.error = "TAR update contains duplicate patches for member: " + patch.name;
            return result;
        }
    }

    std::vector<ParsedMember> members;
    std::map<std::string, std::size_t, std::less<>> target_occurrences;
    std::size_t cursor = 0;
    bool terminated = existing.empty();
    while (cursor < existing.size()) {
        const auto header = existing.subspan(cursor, kRecordSize);
        if (zero_record(header)) {
            terminated = true;
            for (std::size_t trailing = cursor; trailing < existing.size(); trailing += kRecordSize) {
                if (!zero_record(existing.subspan(trailing, kRecordSize))) {
                    result.error = "Existing TAR contains non-zero data after its terminator";
                    return result;
                }
            }
            break;
        }

        std::uint64_t stored_checksum = 0;
        if (!parse_octal(header.subspan(kChecksumOffset, kChecksumSize), stored_checksum) ||
            stored_checksum != header_checksum(header)) {
            result.error = "Existing TAR member header checksum is invalid";
            return result;
        }
        std::uint64_t payload64 = 0;
        if (!parse_octal(header.subspan(kSizeOffset, kSizeSize), payload64) ||
            payload64 > std::numeric_limits<std::size_t>::max()) {
            result.error = "Existing TAR member size field is invalid or unsupported";
            return result;
        }
        const auto payload = static_cast<std::size_t>(payload64);
        if (payload > std::numeric_limits<std::size_t>::max() - (kRecordSize - 1U)) {
            result.error = "Existing TAR member padded size overflows";
            return result;
        }
        const auto padded = ((payload + kRecordSize - 1U) / kRecordSize) * kRecordSize;
        const auto data_offset = cursor + kRecordSize;
        if (data_offset > existing.size() || padded > existing.size() - data_offset) {
            result.error = "Existing TAR member payload extends beyond archive bounds";
            return result;
        }

        const auto name = member_name(header);
        if (name.empty()) {
            result.error = "Existing TAR contains a member with an empty name";
            return result;
        }
        const auto type_byte = std::to_integer<unsigned char>(header[kTypeOffset]);
        const char type = type_byte == 0 ? '\0' : static_cast<char>(type_byte);
        members.push_back({name, cursor, data_offset, payload, padded, type});
        ++result.existing_members;
        if (patch_map.contains(name)) {
            auto& count = target_occurrences[name];
            ++count;
            if (count > 1) {
                result.error = "Existing TAR contains duplicate target member: " + name;
                return result;
            }
            if (type != '\0' && type != '0') {
                result.error = "TAR patch target is not a regular file: " + name;
                return result;
            }
        }
        cursor = data_offset + padded;
    }
    if (!terminated) {
        result.error = "Existing TAR has no zero-block terminator";
        return result;
    }

    std::vector<std::byte> output;
    output.reserve(std::min<std::uint64_t>(options.max_archive_bytes,
                                           existing.size() + 2U * kRecordSize));
    std::map<std::string, bool, std::less<>> applied;
    for (const auto& [name, ignored] : patch_map) {
        (void)ignored;
        applied[name] = false;
    }

    for (const auto& member : members) {
        const auto patch = patch_map.find(member.name);
        if (patch == patch_map.end()) {
            if (!append_bounded(output,
                                existing.subspan(member.header_offset,
                                                 kRecordSize + member.padded_size),
                                options.max_archive_bytes)) {
                result.error = "TAR output exceeds configured byte limit while preserving members";
                return result;
            }
            continue;
        }

        std::array<std::byte, kRecordSize> header{};
        std::copy_n(existing.begin() + static_cast<std::ptrdiff_t>(member.header_offset),
                    kRecordSize, header.begin());
        if (!write_octal(std::span<std::byte>(header).subspan(kSizeOffset, kSizeSize),
                         patch->second->bytes.size()) ||
            !write_checksum(header)) {
            result.error = "Updated TAR member size/checksum cannot be represented: " + member.name;
            return result;
        }
        if (!append_bounded(output, header, options.max_archive_bytes) ||
            !append_payload(output, patch->second->bytes, options.max_archive_bytes)) {
            result.error = "TAR output exceeds configured byte limit while replacing member: " + member.name;
            return result;
        }
        applied[member.name] = true;
        ++result.replaced_members;
    }

    for (const auto& [name, patch] : patch_map) {
        if (applied[name]) {
            continue;
        }
        bool header_ok = false;
        const auto header = new_header(*patch, header_ok);
        if (!header_ok) {
            result.error = "New TAR member cannot be represented in POSIX ustar header: " + name;
            return result;
        }
        if (!append_bounded(output, header, options.max_archive_bytes) ||
            !append_payload(output, patch->bytes, options.max_archive_bytes)) {
            result.error = "TAR output exceeds configured byte limit while adding member: " + name;
            return result;
        }
        ++result.added_members;
    }

    std::array<std::byte, 2U * kRecordSize> terminator{};
    if (!append_bounded(output, terminator, options.max_archive_bytes)) {
        result.error = "TAR output exceeds configured byte limit at terminator";
        return result;
    }

    result.archive = std::move(output);
    result.ok = true;
    return result;
}

} // namespace ps2hdd::tar
