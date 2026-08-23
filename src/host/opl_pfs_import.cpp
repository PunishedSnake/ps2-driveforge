#include "ps2hdd/opl_pfs_import.hpp"

#include <filesystem>
#include <fstream>
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

} // namespace

PreparedPfsImport prepare_pfs_import(std::span<const FetchedAsset> assets,
                                     const PfsImportOptions& options)
{
    PreparedPfsImport result;
    std::set<std::string> destinations;
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

        if (!asset.archive_member.empty()) {
            fail("TAR-layout asset requires a TAR container updater; classic-file importer refused it");
            continue;
        }
        if (asset.target_path.empty()) {
            fail("OPL PFS asset has an empty target path");
            continue;
        }
        if (!destinations.insert(asset.target_path).second) {
            fail("Two staged assets resolve to the same PFS destination");
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

        pfs::BatchFile file;
        file.path = asset.target_path;
        file.bytes = std::move(bytes);
        file.options = options.file_options;
        file.required = true;
        result.files.emplace_back(std::move(file));
    }

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

    pfs::ImageWriter writer(device, partition);
    if (!writer.valid()) {
        result.error = writer.error().empty() ? "PFS image writer rejected OPL import partition"
                                              : writer.error();
        return result;
    }

    result.batch = pfs::write_batch(writer, result.prepared.files);
    if (!result.batch.ok) {
        result.error = result.batch.error.empty() ? "OPL PFS batch import failed"
                                                  : result.batch.error;
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::opl
