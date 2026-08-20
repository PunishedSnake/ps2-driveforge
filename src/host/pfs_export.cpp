#include "ps2hdd/pfs_export.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <set>
#include <string>
#include <system_error>

namespace ps2hdd::pfs {
namespace {

std::string join_path(std::string_view parent, std::string_view child)
{
    if (parent.empty()) {
        return std::string(child);
    }
    std::string result(parent);
    result.push_back('/');
    result.append(child);
    return result;
}

std::filesystem::path utf8_path(std::string_view text)
{
    std::u8string value;
    value.reserve(text.size());
    for (const unsigned char c : text) {
        value.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path(value);
}

std::string uppercase_ascii(std::string_view text)
{
    std::string result(text);
    for (char& c : result) {
        const auto value = static_cast<unsigned char>(c);
        if (value < 0x80) {
            c = static_cast<char>(std::toupper(value));
        }
    }
    return result;
}

bool reserved_windows_name(std::string_view name)
{
    const auto dot = name.find('.');
    const std::string stem = uppercase_ascii(name.substr(0, dot));
    static constexpr std::array<std::string_view, 4> fixed = {"CON", "PRN", "AUX", "NUL"};
    if (std::find(fixed.begin(), fixed.end(), stem) != fixed.end()) {
        return true;
    }
    if (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
        stem[3] >= '1' && stem[3] <= '9') {
        return true;
    }
    return false;
}

std::uint64_t inode_key(BlockInfo location)
{
    return (static_cast<std::uint64_t>(location.subpart) << 32U) | location.number;
}

struct ExportContext {
    Reader& reader;
    ExportResult result;
    ExportProgress progress;
    std::set<std::uint64_t> active_directories;
};

bool export_node(ExportContext& context, const Node& node, std::string_view logical_path,
                 const std::filesystem::path& destination, unsigned depth)
{
    if (depth > 256) {
        context.result.error = "PFS export exceeded the directory-depth safety limit";
        return false;
    }

    const auto mode = node.inode.mode & kModeMask;
    if (mode == kModeRegular) {
        std::error_code ec;
        if (!destination.parent_path().empty()) {
            std::filesystem::create_directories(destination.parent_path(), ec);
            if (ec) {
                context.result.error = "Could not create host output directory: " + ec.message();
                return false;
            }
        }

        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) {
            context.result.error = "Could not create host output file";
            return false;
        }

        constexpr std::size_t kChunkSize = 1024 * 1024;
        std::vector<std::byte> buffer(kChunkSize);
        std::uint64_t offset = 0;
        while (offset < node.inode.size) {
            const auto take = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), node.inode.size - offset));
            auto chunk = std::span<std::byte>(buffer.data(), take);
            if (!context.reader.read(node, offset, chunk)) {
                context.result.error = context.reader.last_error();
                output.close();
                std::filesystem::remove(destination, ec);
                return false;
            }
            output.write(reinterpret_cast<const char*>(chunk.data()),
                         static_cast<std::streamsize>(take));
            if (!output) {
                context.result.error = "Host write failed during PFS export";
                output.close();
                std::filesystem::remove(destination, ec);
                return false;
            }
            offset += take;
            context.result.stats.bytes += take;
            if (context.progress) {
                context.progress(context.result.stats, logical_path);
            }
        }
        ++context.result.stats.files;
        if (context.progress) {
            context.progress(context.result.stats, logical_path);
        }
        return true;
    }

    if (mode != kModeDirectory) {
        ++context.result.stats.skipped;
        context.result.warnings.emplace_back("Skipped unsupported PFS node: " + std::string(logical_path));
        return true;
    }

    const auto key = inode_key(node.location);
    if (!context.active_directories.insert(key).second) {
        context.result.error = "PFS directory cycle detected while exporting: " + std::string(logical_path);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    if (ec) {
        context.active_directories.erase(key);
        context.result.error = "Could not create host directory: " + ec.message();
        return false;
    }
    ++context.result.stats.directories;
    if (context.progress) {
        context.progress(context.result.stats, logical_path);
    }

    const auto entries = context.reader.list_directory(node);
    if (!context.reader.last_error().empty()) {
        context.active_directories.erase(key);
        context.result.error = context.reader.last_error();
        return false;
    }

    std::set<std::string> host_names;
    for (const auto& entry : entries) {
        auto child = context.reader.read_inode(entry.inode);
        if (!child) {
            ++context.result.stats.skipped;
            context.result.warnings.emplace_back("Could not read inode for: " +
                                                 join_path(logical_path, entry.name));
            continue;
        }

        std::string host_name = sanitize_host_filename(entry.name);
        const std::string base_name = host_name;
        unsigned suffix = 2;
        while (!host_names.insert(uppercase_ascii(host_name)).second) {
            host_name = base_name + "_" + std::to_string(suffix++);
        }

        const std::string child_path = join_path(logical_path, entry.name);
        if (!export_node(context, *child, child_path,
                         destination / utf8_path(host_name), depth + 1)) {
            context.active_directories.erase(key);
            return false;
        }
    }

    context.active_directories.erase(key);
    return true;
}

} // namespace

std::string sanitize_host_filename(std::string_view name)
{
    std::string result;
    result.reserve(name.size());
    for (const unsigned char c : name) {
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*') {
            result.push_back('_');
        } else {
            result.push_back(static_cast<char>(c));
        }
    }

    if (result.empty() || result == "." || result == "..") {
        result = "_";
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
        result.back() = '_';
    }
    if (reserved_windows_name(result)) {
        result.insert(result.begin(), '_');
    }
    return result;
}

ExportResult export_to_host(Reader& reader, std::string_view source_path,
                            const std::filesystem::path& destination,
                            ExportProgress progress)
{
    ExportContext context{reader, {}, std::move(progress), {}};
    auto node = reader.resolve(source_path);
    if (!node) {
        context.result.error = reader.last_error();
        return context.result;
    }

    context.result.ok = export_node(context, *node, source_path, destination, 0);
    return context.result;
}

} // namespace ps2hdd::pfs
