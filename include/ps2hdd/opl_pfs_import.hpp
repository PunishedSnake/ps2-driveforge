#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/opl_asset_pipeline.hpp"
#include "ps2hdd/pfs_batch.hpp"
#include "ps2hdd/tar_archive.hpp"
#include "ps2hdd/writable_block_device.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::opl {

struct PfsImportOptions {
    std::size_t max_file_bytes{64U * 1024U * 1024U};
    std::uint64_t max_archive_bytes{256ULL * 1024ULL * 1024ULL};
    pfs::FileWriteOptions file_options{};
    pfs::DirectoryWriteOptions directory_options{};
};

struct PfsImportIssue {
    AssetKind kind{};
    std::string target_path;
    bool fatal{};
    std::string message;
};

struct PreparedTarArchive {
    std::string path;
    AssetKind representative_kind{};
    std::vector<tar::MemberPatch> members;
};

struct PreparedPfsImport {
    bool ok{};
    std::string error;
    std::vector<std::string> directories;
    std::vector<pfs::BatchFile> files;
    std::vector<PreparedTarArchive> archives;
    std::vector<PfsImportIssue> issues;
    std::size_t cache_only_skipped{};
    std::size_t non_pfs_skipped{};
    std::uint64_t total_bytes{};
};

struct TarImportResult {
    bool ok{};
    std::string error;
    std::string path;
    std::size_t replaced_members{};
    std::size_t added_members{};
    pfs::FileWriteResult write;
};

struct PfsImportResult {
    bool ok{};
    bool partial{};
    std::string error;
    PreparedPfsImport prepared;
    std::vector<pfs::DirectoryEnsureResult> directories;
    pfs::BatchWriteResult batch;
    std::vector<TarImportResult> archives;
};

// Convert already-downloaded provider output into an immutable PFS mutation
// plan before any disk write starts. Classic files become one batch. TAR-layout
// assets are grouped by container path and validated as distinct archive members.
// A classic file and TAR container may never claim the same PFS destination.
[[nodiscard]] PreparedPfsImport prepare_pfs_import(
    std::span<const FetchedAsset> assets,
    const PfsImportOptions& options = {});

// Execute the complete image-only OPL import through one ImageWriter session.
// Missing parent directories are ensured first, classic files are written as a
// batch, then TAR containers are read/rebuilt and copy-on-write replaced. A TAR
// failure after earlier files committed is surfaced as partial=true rather than
// pretending the whole multi-file import was globally atomic.
[[nodiscard]] PfsImportResult import_fetched_assets(
    WritableBlockDevice& device,
    const apa::Partition& partition,
    std::span<const FetchedAsset> assets,
    const PfsImportOptions& options = {});

} // namespace ps2hdd::opl
