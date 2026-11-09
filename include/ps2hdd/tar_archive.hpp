#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ps2hdd::tar {

struct MemberPatch {
    std::string name;
    std::vector<std::byte> bytes;
};

struct UpdateOptions {
    std::uint64_t max_archive_bytes{256ULL * 1024ULL * 1024ULL};
};

struct UpdateResult {
    bool ok{};
    std::string error;
    std::vector<std::byte> archive;
    std::size_t existing_members{};
    std::size_t replaced_members{};
    std::size_t added_members{};
};

// Rebuild a classic 512-byte-record TAR while preserving every untouched
// member header and payload byte-for-byte. Patched regular files keep their
// existing metadata except size/checksum; missing members receive deterministic
// POSIX ustar headers. Malformed archives, ambiguous target duplicates and path
// traversal are refused before an output archive is returned.
//
// An empty input span is a supported way to create a new archive. The result
// always ends in two zero records and is bounded by max_archive_bytes.
[[nodiscard]] UpdateResult update_archive(
    std::span<const std::byte> existing,
    std::span<const MemberPatch> patches,
    const UpdateOptions& options = {});

} // namespace ps2hdd::tar
