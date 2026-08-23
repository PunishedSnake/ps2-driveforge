#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/opl_asset_pipeline.hpp"
#include "ps2hdd/pfs_batch.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::opl {

struct PfsImportOptions {
    std::size_t max_file_bytes{64U * 1024U * 1024U};
    pfs::FileWriteOptions file_options{};
    pfs::DirectoryWriteOptions directory_options{};
};

struct PfsImportIssue {
    AssetKind kind{};
    std::string target_path;
    bool fatal{};
    std::string message;
};

struct PreparedPfsImport {
    bool ok{};
    std::string error;
    std::vector<std::string> directories;
    std::vector<pfs::BatchFile> files;
    std::vector<PfsImportIssue> issues;
    std::size_t cache_only_skipped{};
    std::size_t non_pfs_skipped{};
    std::uint64_t total_bytes{};
};

struct PfsImportResult {
    bool ok{};
    std::string error;
    PreparedPfsImport prepared;
    std::vector<pfs::DirectoryEnsureResult> directories;
    pfs::BatchWriteResult batch;
};

// Convert already-downloaded provider output into an immutable PFS batch before
// any disk mutation starts. This layer validates host files, byte limits,
// destination uniqueness and placement policy and derives the exact parent
// directory set in shallow-to-deep order. TAR members are refused until a real
// TAR container updater exists; silently writing them as normal files would be
// exactly the sort of convenient lie that corrupts installations.
[[nodiscard]] PreparedPfsImport prepare_pfs_import(
    std::span<const FetchedAsset> assets,
    const PfsImportOptions& options = {});

// Execute one prepared classic-file OPL import through a single ImageWriter
// session. Missing parent directories are ensured first, then all files are
// written through one cached batch session. PhysicalDrive cannot reach this API
// without a WritableBlockDevice capability.
[[nodiscard]] PfsImportResult import_fetched_assets(
    WritableBlockDevice& device,
    const apa::Partition& partition,
    std::span<const FetchedAsset> assets,
    const PfsImportOptions& options = {});

} // namespace ps2hdd::opl
