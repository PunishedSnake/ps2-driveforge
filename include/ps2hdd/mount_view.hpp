#pragma once

#include "ps2hdd/drive_session.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd {

enum class MountNodeKind {
    root,
    namespace_directory,
    pfs_directory,
    pfs_file,
    // Read-only virtual ISO assembled from an HDLoader game's allocation table.
    hdl_iso,
};

[[nodiscard]] constexpr bool mount_kind_is_directory(MountNodeKind kind) noexcept
{
    return kind == MountNodeKind::root || kind == MountNodeKind::namespace_directory ||
           kind == MountNodeKind::pfs_directory;
}

struct MountEntry {
    std::string name;
    MountNodeKind kind{MountNodeKind::pfs_file};
    std::uint64_t size{};

    [[nodiscard]] bool is_directory() const noexcept
    {
        return mount_kind_is_directory(kind);
    }
};

struct MountLookupResult {
    bool ok{};
    std::string error;
    MountNodeKind kind{MountNodeKind::root};
    std::string partition;
    std::string pfs_path;
    std::uint64_t size{};

    [[nodiscard]] bool is_directory() const noexcept
    {
        return mount_kind_is_directory(kind);
    }
};

struct MountListResult {
    bool ok{};
    std::string error;
    std::vector<MountEntry> entries;
};

struct MountReadResult {
    bool ok{};
    std::string error;
    std::size_t bytes_read{};
};

// Portable view of the filesystem namespace presented by Darkness. Dokany is
// intentionally only an adapter on top of this class, which keeps Windows driver
// callback code out of path mapping and makes Explorer semantics testable under
// the normal Linux sanitizer job.
//
// Initial namespace:
//   \ (root)
//   \Partitions
//   \Partitions\<PFS partition>
//   \Partitions\<PFS partition>\<PFS tree>
//   \Games\<title> [<startup>].iso      (Frieren: HDL virtual ISOs)
//
// PFS and APA names are mapped to deterministic Windows-safe aliases. The
// original names remain untouched on disk and are used internally when calling
// DriveSession.
class ReadOnlyMountView {
public:
    explicit ReadOnlyMountView(DriveSession& session) : session_(session) {}

    [[nodiscard]] MountLookupResult lookup(std::string_view path);
    [[nodiscard]] MountListResult list_directory(std::string_view path);
    [[nodiscard]] MountReadResult read_file(std::string_view path, std::uint64_t offset,
                                            std::span<std::byte> out);

    struct GameAlias {
        std::string visible;
        std::string partition;
        std::uint64_t size{};
    };

private:
    // Built once per mount: Dokany performs a lookup for every callback, and
    // the HDL alias table only depends on the immutable session scan.
    [[nodiscard]] std::shared_ptr<const std::vector<GameAlias>> games();

    DriveSession& session_;
    std::mutex games_mutex_;
    std::shared_ptr<const std::vector<GameAlias>> games_;
};

} // namespace ps2hdd
