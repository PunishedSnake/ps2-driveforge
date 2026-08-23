#include "ps2hdd/tar_archive.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::byte> bytes(std::string_view text)
{
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

std::optional<std::uint64_t> octal(std::span<const std::byte> field)
{
    std::uint64_t value = 0;
    bool digit = false;
    for (const auto byte : field) {
        const auto ch = std::to_integer<unsigned char>(byte);
        if (ch == 0 || ch == static_cast<unsigned char>(' ')) {
            if (digit) {
                break;
            }
            continue;
        }
        if (ch < static_cast<unsigned char>('0') || ch > static_cast<unsigned char>('7')) {
            return std::nullopt;
        }
        digit = true;
        value = value * 8U + (ch - static_cast<unsigned char>('0'));
    }
    return value;
}

struct RawMember {
    std::size_t offset{};
    std::size_t padded_bytes{};
    std::vector<std::byte> payload;
};

std::optional<RawMember> find_member(std::span<const std::byte> archive,
                                     std::string_view wanted)
{
    constexpr std::size_t record = 512;
    std::size_t cursor = 0;
    while (cursor + record <= archive.size()) {
        const auto header = archive.subspan(cursor, record);
        if (std::all_of(header.begin(), header.end(), [](std::byte value) {
                return value == std::byte{0};
            })) {
            return std::nullopt;
        }
        std::size_t name_length = 0;
        while (name_length < 100 && header[name_length] != std::byte{0}) {
            ++name_length;
        }
        const std::string name(reinterpret_cast<const char*>(header.data()), name_length);
        const auto size = octal(header.subspan(124, 12));
        if (!size) {
            return std::nullopt;
        }
        const auto payload_size = static_cast<std::size_t>(*size);
        const auto padded = ((payload_size + record - 1U) / record) * record;
        if (cursor + record > archive.size() ||
            padded > archive.size() - (cursor + record)) {
            return std::nullopt;
        }
        if (name == wanted) {
            RawMember result;
            result.offset = cursor;
            result.padded_bytes = record + padded;
            result.payload.assign(archive.begin() + static_cast<std::ptrdiff_t>(cursor + record),
                                  archive.begin() + static_cast<std::ptrdiff_t>(cursor + record + payload_size));
            return result;
        }
        cursor += record + padded;
    }
    return std::nullopt;
}

void create_and_replace_members()
{
    const std::vector<ps2hdd::tar::MemberPatch> initial{
        {"SLUS_123.45.cfg", bytes("Title=First\nMode=1\n")},
        {"SLUS_123.45.cht", bytes("gametitle=Test\n")},
    };
    const auto created = ps2hdd::tar::update_archive({}, initial);
    check(created.ok, "new TAR builds");
    check(created.added_members == 2 && created.replaced_members == 0,
          "new TAR reports added members");
    check(created.archive.size() % 512U == 0,
          "new TAR remains record aligned");

    const auto cfg_before = find_member(created.archive, "SLUS_123.45.cfg");
    const auto cht_before = find_member(created.archive, "SLUS_123.45.cht");
    check(cfg_before && cht_before, "new TAR members can be located");

    const std::vector<ps2hdd::tar::MemberPatch> replacement{
        {"SLUS_123.45.cfg", bytes("Title=Second\nMode=3\nDMA=7\n")},
    };
    const auto updated = ps2hdd::tar::update_archive(created.archive, replacement);
    check(updated.ok, "existing TAR member replaces");
    check(updated.replaced_members == 1 && updated.added_members == 0,
          "replacement accounting is exact");
    const auto cfg_after = find_member(updated.archive, "SLUS_123.45.cfg");
    const auto cht_after = find_member(updated.archive, "SLUS_123.45.cht");
    check(cfg_after && cfg_after->payload == replacement.front().bytes,
          "replaced member payload is exact");
    check(cht_after && cht_after->payload == cht_before->payload,
          "untouched member payload is preserved");

    const auto untouched_before = std::span<const std::byte>(created.archive).subspan(
        cht_before->offset, cht_before->padded_bytes);
    const auto untouched_after = std::span<const std::byte>(updated.archive).subspan(
        cht_after->offset, cht_after->padded_bytes);
    check(std::equal(untouched_before.begin(), untouched_before.end(), untouched_after.begin()),
          "untouched TAR record is preserved byte-for-byte");

    const auto validation = ps2hdd::tar::update_archive(updated.archive, {});
    check(validation.ok && validation.existing_members == 2,
          "rebuilt TAR reparses without patches");
}

void malformed_and_ambiguous_input_is_refused()
{
    const std::vector<ps2hdd::tar::MemberPatch> one{{"ART/cover.png", bytes("png")}};
    const auto valid = ps2hdd::tar::update_archive({}, one);
    check(valid.ok, "fixture TAR builds");

    auto corrupt = valid.archive;
    corrupt[148] ^= std::byte{1};
    const auto bad_checksum = ps2hdd::tar::update_archive(corrupt, {});
    check(!bad_checksum.ok, "bad TAR checksum is refused");

    const std::vector<ps2hdd::tar::MemberPatch> duplicate{
        {"same.cfg", bytes("a")},
        {"same.cfg", bytes("b")},
    };
    check(!ps2hdd::tar::update_archive({}, duplicate).ok,
          "duplicate patch target is refused");

    const std::vector<ps2hdd::tar::MemberPatch> traversal{
        {"../escape.cfg", bytes("x")},
    };
    check(!ps2hdd::tar::update_archive({}, traversal).ok,
          "TAR traversal target is refused");
}

void output_limit_is_enforced_before_returning_archive()
{
    ps2hdd::tar::UpdateOptions options;
    options.max_archive_bytes = 1536;
    const std::vector<ps2hdd::tar::MemberPatch> patch{
        {"large.bin", std::vector<std::byte>(700, std::byte{0x5A})},
    };
    const auto result = ps2hdd::tar::update_archive({}, patch, options);
    check(!result.ok && result.archive.empty(),
          "TAR byte limit refuses oversized rebuilt archive");
}

} // namespace

int main()
{
    try {
        create_and_replace_members();
        malformed_and_ambiguous_input_is_refused();
        output_limit_is_enforced_before_returning_archive();
        std::cout << "TAR archive tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TAR archive test failure: " << error.what() << '\n';
        return 1;
    }
}
