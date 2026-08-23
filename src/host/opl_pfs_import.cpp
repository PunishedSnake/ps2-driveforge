#include "ps2hdd/opl_pfs_import.hpp"

#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/pfs.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <system_error>
#include <utility>

namespace ps2hdd::opl {
namespace {

std::vector<std::byte> read_file(const std::filesystem::path& path, std::size_t size,
                                 std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not open staged host asset";
        return {};
    }

    std::vector<std::byte> bytes(size);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) {
            error = "Could not read the complete staged host asset";
            return {};
        }
    }
    return bytes;
}

std::string normalize_target_path(std::string_view input, std::string& error)
{
    std::string out;
    out.reserve(input.size());
    bool previous_slash = true;
    for (const char ch : input) {
        const char normalized = ch == '\\' ? '/' : ch;
        if (normalized == '/') {
            if (!previous_slash) {
                out.push_back('/');
            }
            previous_slash = true;
        } else {
            out.push_back(normalized);
            previous_slash = false;
        }
    }
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    if (out.empty()) {
        error = "OPL PFS asset has an empty target path";
        return {};
    }

    std::size_t cursor = 0;
    while (cursor < out.size()) {
        const auto slash = out.find('/', cursor);
        const auto end = slash == std::string::npos ? out.size() : slash;
        const auto component = std::string_view(out).substr(cursor, end - cursor);
        if (component.empty() || component == "." || component == ".." || component.size() > 255) {
            error = "OPL PFS asset target contains an invalid path component";
            return {};
        }
        if (slash == std::string::npos) {
            break;
        }
        cursor = slash + 1U;
    }
    return out;
}

void add_parent_directories(std::string_view path, std::set<std::string>& directories)
{
    std::size_t slash = path.find('/');
    while (slash != std::string_view::npos) {
        directories.emplace(path.substr(0, slash));
        slash = path.find('/', slash + 1U);
    }
}

std::size_t depth(std::string_view path) noexcept
{
    return static_cast<std::size_t>(std::count(path.begin(), path.end(), '/')) + 1U;
}

bool missing_path_error(std::string_view error) noexcept
{
    return error.rfind("PFS path component not found:", 0) == 0;
}

struct BuiltTar {
    const PreparedTarArchive* plan{};
    tar::UpdateResult update;
};

} // namespace

PreparedPfsImport prepare_pfs_import(std::span<const FetchedAsset> assets,
                                     const PfsImportOptions& options)
{
    PreparedPfsImport result;
    std::set<std::string> direct_destinations;
    std::map<std::string, std::size_t, std::less<>> archive_indices;
    std::set<std::pair<std::string, std::string>> archive_members;
    std::set<std::string> directories;
    bool fatal = false;

    for (const auto& asset : assets) {
        if (asset.placement == Placement::cache_only) {
            ++result.cache_only_skipped;
            continue;
        }
        if (asset.placement == Placement::hdd_osd_metadata) {
            ++result.non_pfs_skipped;
            result.issues.push_back({asset.kind, asset.target_path, false,
                                     "HDD-OSD metadata is outside the OPL PFS import surface"});
            continue;
        }

        auto fail = [&](std::string message) {
            fatal = true;
            result.issues.push_back({asset.kind, asset.target_path, true, std::move(message)});
        };

        std::string path_error;
        auto target_path = normalize_target_path(asset.target_path, path_error);
        if (!path_error.empty()) {
            fail(std::move(path_error));
            continue;
        }

        std::error_code ec;
        const auto host_size = std::filesystem::file_size(asset.staged_path, ec);
        if (ec) {
            fail("Staged host asset is missing or unreadable");
            continue;
        }
        if (host_size > options.max_file_bytes) {
            fail("Staged host asset exceeds the configured PFS import byte limit");
            continue;
        }

        std::string read_error;
        auto bytes = read_file(asset.staged_path, static_cast<std::size_t>(host_size), read_error);
        if (!read_error.empty()) {
            fail(std::move(read_error));
            continue;
        }
        if (result.total_bytes > std::numeric_limits<std::uint64_t>::max() - bytes.size()) {
            fail("PFS import byte accounting overflow");
            continue;
        }
        result.total_bytes += bytes.size();
        add_parent_directories(target_path, directories);

        if (asset.archive_member.empty()) {
            if (archive_indices.contains(target_path)) {
                fail("Classic file collides with a TAR container at the same PFS destination");
                continue;
            }
            if (!direct_destinations.insert(target_path).second) {
                fail("Two staged assets resolve to the same classic PFS destination");
                continue;
            }

            pfs::BatchFile file;
            file.path = std::move(target_path);
            file.bytes = std::move(bytes);
            file.options = options.file_options;
            file.required = true;
            result.files.emplace_back(std::move(file));
            continue;
        }

        if (direct_destinations.contains(target_path)) {
            fail("TAR container collides with a classic file at the same PFS destination");
            continue;
        }
        if (!archive_members.emplace(target_path, asset.archive_member).second) {
            fail("Two staged assets resolve to the same TAR member");
            continue;
        }

        std::size_t archive_index = 0;
        const auto found = archive_indices.find(target_path);
        if (found == archive_indices.end()) {
            archive_index = result.archives.size();
            archive_indices.emplace(target_path, archive_index);
            PreparedTarArchive archive;
            archive.path = target_path;
            archive.representative_kind = asset.kind;
            result.archives.emplace_back(std::move(archive));
        } else {
            archive_index = found->second;
        }
        result.archives[archive_index].members.push_back({asset.archive_member, std::move(bytes)});
    }

    tar::UpdateOptions tar_options;
    tar_options.max_archive_bytes = options.max_archive_bytes;
    for (const auto& archive : result.archives) {
        const auto validation = tar::update_archive({}, archive.members, tar_options);
        if (!validation.ok) {
            fatal = true;
            result.issues.push_back({archive.representative_kind, archive.path, true,
                                     "TAR preflight failed: " + validation.error});
        }
    }

    result.directories.assign(directories.begin(), directories.end());
    std::sort(result.directories.begin(), result.directories.end(), [](const auto& left, const auto& right) {
        const auto left_depth = depth(left);
        const auto right_depth = depth(right);
        return left_depth != right_depth ? left_depth < right_depth : left < right;
    });

    result.ok = !fatal;
    if (fatal) {
        result.error = "Staged OPL assets failed PFS import preflight; zero PFS writes performed";
    }
    return result;
}

PfsImportResult import_fetched_assets(WritableBlockDevice& device,
                                      const apa::Partition& partition,
                                      std::span<const FetchedAsset> assets,
                                      const PfsImportOptions& options)
{
    PfsImportResult result;
    result.prepared = prepare_pfs_import(assets, options);
    if (!result.prepared.ok) {
        result.error = result.prepared.error;
        return result;
    }
    if (partition.type != apa::kTypePfs || partition.is_sub()) {
        result.error = "OPL asset import requires a main APA PFS partition";
        return result;
    }

    // Freeze and validate every existing TAR before the first filesystem write.
    // This avoids committing classic assets only to discover afterwards that an
    // existing ART/cfg/cht container was malformed or over the configured limit.
    ApaVolume read_volume(device, partition);
    pfs::Reader reader(read_volume);
    if (!reader.valid()) {
        result.error = reader.last_error().empty() ? "PFS reader rejected OPL import partition"
                                                   : reader.last_error();
        return result;
    }

    tar::UpdateOptions tar_options;
    tar_options.max_archive_bytes = options.max_archive_bytes;
    std::vector<BuiltTar> built_archives;
    built_archives.reserve(result.prepared.archives.size());
    for (const auto& archive : result.prepared.archives) {
        std::vector<std::byte> existing;
        const auto node = reader.resolve(archive.path);
        if (node) {
            if ((node->inode.mode & pfs::kModeMask) != pfs::kModeRegular) {
                result.error = "OPL TAR destination already exists but is not a regular file: " + archive.path;
                return result;
            }
            if (node->inode.size > options.max_archive_bytes ||
                node->inode.size > std::numeric_limits<std::size_t>::max()) {
                result.error = "Existing OPL TAR exceeds configured archive byte limit: " + archive.path;
                return result;
            }
            existing.resize(static_cast<std::size_t>(node->inode.size));
            if (!existing.empty() && !reader.read(*node, 0, existing)) {
                result.error = "Could not read existing OPL TAR " + archive.path + ": " +
                               reader.last_error();
                return result;
            }
        } else if (!missing_path_error(reader.last_error())) {
            result.error = "Could not resolve OPL TAR destination " + archive.path + ": " +
                           reader.last_error();
            return result;
        }

        auto update = tar::update_archive(existing, archive.members, tar_options);
        if (!update.ok) {
            result.error = "Could not rebuild OPL TAR " + archive.path + ": " + update.error;
            return result;
        }
        built_archives.push_back({&archive, std::move(update)});
    }

    pfs::ImageWriter writer(device, partition);
    if (!writer.valid()) {
        result.error = writer.error().empty() ? "PFS image writer rejected OPL import partition"
                                              : writer.error();
        return result;
    }

    bool mutated = false;
    result.directories.reserve(result.prepared.directories.size());
    for (const auto& directory : result.prepared.directories) {
        auto ensured = writer.ensure_directory_full(directory, options.directory_options);
        const bool ok = ensured.ok;
        mutated = mutated || ensured.created_components != 0;
        if (!ok && result.error.empty()) {
            result.error = "Could not prepare OPL PFS directory " + directory + ": " + ensured.error;
        }
        result.directories.emplace_back(std::move(ensured));
        if (!ok) {
            result.partial = mutated;
            return result;
        }
    }

    result.batch = pfs::write_batch(writer, result.prepared.files);
    mutated = mutated || result.batch.succeeded != 0;
    if (!result.batch.ok) {
        result.error = result.batch.error.empty() ? "OPL PFS batch import failed"
                                                  : result.batch.error;
        result.partial = mutated;
        return result;
    }

    result.archives.reserve(built_archives.size());
    for (auto& built : built_archives) {
        TarImportResult archive_result;
        archive_result.path = built.plan->path;
        archive_result.replaced_members = built.update.replaced_members;
        archive_result.added_members = built.update.added_members;
        archive_result.write = writer.write_file_complete(
            built.plan->path, built.update.archive, options.file_options);
        if (!archive_result.write.ok) {
            archive_result.error = archive_result.write.error.empty()
                                       ? "PFS writer failed while publishing rebuilt TAR"
                                       : archive_result.write.error;
            result.error = "Could not publish OPL TAR " + built.plan->path + ": " +
                           archive_result.error;
            result.archives.emplace_back(std::move(archive_result));
            result.partial = mutated;
            return result;
        }
        archive_result.ok = true;
        mutated = true;
        result.archives.emplace_back(std::move(archive_result));
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::opl
