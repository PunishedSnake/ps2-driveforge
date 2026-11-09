#pragma once

#include "ps2hdd/pfs_write.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::pfs {

struct BatchFile {
    std::string path;
    std::vector<std::byte> bytes;
    FileWriteOptions options{};
    bool required{true};
};

struct BatchFileResult {
    std::size_t index{};
    std::string path;
    bool required{true};
    FileWriteResult write;
};

struct BatchWriteOptions {
    // Required failures stop the batch by default. Already committed files are
    // deliberately not rolled back: each file has its own crash-safe publication
    // sequence and a later recovery layer can reason about exact completed items.
    bool stop_on_required_failure{true};
};

struct BatchWriteResult {
    bool ok{};
    bool partial{};
    std::string error;
    std::vector<BatchFileResult> files;
    std::size_t attempted{};
    std::size_t succeeded{};
    std::size_t failed{};
    std::size_t optional_failed{};
    std::uint64_t bytes_committed{};
    std::size_t metadata_transactions{};
    std::size_t payload_write_calls{};
    std::size_t bitmap_chunks_touched{};
};

// Execute many file mutations through one already-probed ImageWriter session.
// This is the fast path for OPL asset imports: the PFS probe and lazy bitmap
// cache survive between items instead of being rebuilt by a shell/process per
// file. The batch itself is intentionally not one giant transaction. Each item
// is independently verified and published so a large import does not require a
// filesystem-sized before-image in RAM.
[[nodiscard]] BatchWriteResult write_batch(ImageWriter& writer,
                                           std::span<const BatchFile> files,
                                           const BatchWriteOptions& options = {});

} // namespace ps2hdd::pfs
