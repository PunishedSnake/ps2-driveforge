#pragma once

#include "ps2hdd/magicgate_cipher.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ps2hdd::magicgate {

struct DiskKeyset {
    // No Sony key material is compiled into DriveForge. These values are an
    // injected capability loaded from user-controlled local configuration.
    cipher::Block kbit_material{};
    cipher::Block kc_material{};
    cipher::DoubleKey kbit_master{};
    cipher::DoubleKey kc_master{};
};

struct SigningKeyset {
    cipher::Block signature_master{};
    cipher::Block signature_hash{};
    cipher::Block root_signature_master{};
    cipher::DoubleKey root_signature_hash{};
    cipher::Block content_table_iv{};
    cipher::Block content_iv{};
};

struct MagicGateKeyset {
    DiskKeyset disk;
    SigningKeyset signing;
};

struct KeysetParseResult {
    bool ok{};
    std::size_t line{};
    std::string error;
    MagicGateKeyset keyset;
};

namespace keyset_detail {

[[nodiscard]] constexpr bool whitespace(char ch) noexcept
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
           ch == '\f' || ch == '\v';
}

[[nodiscard]] constexpr std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && whitespace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && whitespace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] constexpr int hex_nibble(char ch) noexcept
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

template <std::size_t N>
[[nodiscard]] constexpr bool parse_hex_exact(std::string_view text,
                                             std::array<std::byte, N>& out) noexcept
{
    text = trim(text);
    if (text.size() != N * 2U) {
        return false;
    }
    for (std::size_t i = 0; i < N; ++i) {
        const int high = hex_nibble(text[i * 2U]);
        const int low = hex_nibble(text[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}

[[nodiscard]] constexpr bool assign_key(std::string_view name,
                                        std::string_view value,
                                        MagicGateKeyset& keyset,
                                        std::array<bool, 10>& seen,
                                        std::string& error)
{
    const auto duplicate = [&](std::size_t index) constexpr {
        if (seen[index]) {
            error = "duplicate MagicGate keyset entry";
            return true;
        }
        seen[index] = true;
        return false;
    };

    if (name == "MG_SIG_MASTER_KEY") {
        if (duplicate(0)) return false;
        if (!parse_hex_exact(value, keyset.signing.signature_master)) {
            error = "MG_SIG_MASTER_KEY must contain exactly 8 bytes of hexadecimal";
            return false;
        }
        return true;
    }
    if (name == "MG_SIG_HASH_KEY") {
        if (duplicate(1)) return false;
        if (!parse_hex_exact(value, keyset.signing.signature_hash)) {
            error = "MG_SIG_HASH_KEY must contain exactly 8 bytes of hexadecimal";
            return false;
        }
        return true;
    }
    if (name == "MG_KBIT_MASTER_KEY") {
        if (duplicate(2)) return false;
        if (!parse_hex_exact(value, keyset.disk.kbit_master)) {
            error = "MG_KBIT_MASTER_KEY must contain exactly 16 bytes of hexadecimal";
            return false;
        }
        return true;
    }
    if (name == "MG_KBIT_IV" || name == "MG_KBIT_MATERIAL") {
        if (duplicate(3)) return false;
        if (!parse_hex_exact(value, keyset.disk.kbit_material)) {
            error = "MG_KBIT_IV/MG_KBIT_MATERIAL must contain exactly 8 bytes of hexadecimal";
            return false;
        }
        return true;
    }
    if (name == "MG_KC_MASTER_KEY") {
        if (duplicate(4)) return false;
        if (!parse_hex_exact(value, keyset.disk.kc_master)) {
            error = "MG_KC_MASTER_KEY must contain exactly 16 bytes of hexadecimal";
            return false;
        }
        return true;
    }
    if (name == "MG_KC_IV" || name == "MG_KC_MATERIAL") {
        if (duplicate(5)) return false;
        if (!parse_hex_exact(value, keyset.disk.kc_material)) {
            error = "MG_KC_IV/MG_KC_MATERIAL must contain exactly 8 bytes of hexadecimal";
            return false;
        }
        return true;
    }
    if (name == "MG_ROOTSIG_MASTER_KEY") {
        if (duplicate(6)) return false;
        if (!parse_hex_exact(value, keyset.signing.root_signature_master)) {
            error = "MG_ROOTSIG_MASTER_KEY must contain exactly 8 bytes of hexadecimal";
            return false;
        }
        return true;
    }
    if (name == "MG_ROOTSIG_HASH_KEY") {
        if (duplicate(7)) return false;
        if (!parse_hex_exact(value, keyset.signing.root_signature_hash)) {
            error = "MG_ROOTSIG_HASH_KEY must contain exactly 16 bytes of hexadecimal";
            return false;
        }
        return true;
    }
    if (name == "MG_CONTENT_TABLE_IV") {
        if (duplicate(8)) return false;
        if (!parse_hex_exact(value, keyset.signing.content_table_iv)) {
            error = "MG_CONTENT_TABLE_IV must contain exactly 8 bytes of hexadecimal";
            return false;
        }
        return true;
    }
    if (name == "MG_CONTENT_IV") {
        if (duplicate(9)) return false;
        if (!parse_hex_exact(value, keyset.signing.content_iv)) {
            error = "MG_CONTENT_IV must contain exactly 8 bytes of hexadecimal";
            return false;
        }
        return true;
    }

    error = "unknown MagicGate keyset entry: " + std::string(name);
    return false;
}

} // namespace keyset_detail

// Parse the de-facto key=value format used by PC KELF tooling. File I/O is
// intentionally kept outside this routine so callers can impose their own
// bounded-read and provenance policy. Blank lines and lines beginning with '#'
// or ';' are ignored. Every required capability must appear exactly once.
[[nodiscard]] constexpr KeysetParseResult parse_magicgate_keyset(std::string_view text)
{
    KeysetParseResult result;
    std::array<bool, 10> seen{};
    std::size_t line_number = 0;
    std::size_t cursor = 0;

    while (cursor <= text.size()) {
        ++line_number;
        const auto newline = text.find('\n', cursor);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        auto line = keyset_detail::trim(text.substr(cursor, end - cursor));
        cursor = newline == std::string_view::npos ? text.size() + 1U : newline + 1U;

        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos || line.find('=', equals + 1U) != std::string_view::npos) {
            result.line = line_number;
            result.error = "MagicGate keyset line must contain exactly one '='";
            return result;
        }

        const auto name = keyset_detail::trim(line.substr(0, equals));
        const auto value = keyset_detail::trim(line.substr(equals + 1U));
        if (name.empty() || value.empty()) {
            result.line = line_number;
            result.error = "MagicGate keyset name and value must be non-empty";
            return result;
        }

        if (!keyset_detail::assign_key(name, value, result.keyset, seen, result.error)) {
            result.line = line_number;
            return result;
        }
    }

    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (!seen[i]) {
            result.error = "MagicGate keyset is missing one or more required entries";
            return result;
        }
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::magicgate
