#pragma once

#include "ps2hdd/mount_view.hpp"
#include "ps2hdd/pfs_write_back.hpp"

#include <string>
#include <string_view>

namespace ps2hdd {

// Explorer-facing namespace with write-back overlay. Everything the read-only
// view shows is still served by ReadOnlyMountView; the overlay only adds,
// resizes or hides PFS entries that have pending (uncommitted) changes, so a
// user sees their own copy/delete/rename immediately while the engine batches
// the real disk work in the background.
class WritableMountView {
public:
    WritableMountView(DriveSession& session, PfsWriteBack& engine)
        : base_(session), engine_(engine)
    {
    }

    struct Target {
        bool ok{};
        std::string error;
        std::string partition;
        std::string pfs_path;
        bool exists{};
        bool is_directory{};
        std::uint64_t size{};
        bool writable{};
    };

    // Maps a Windows mount path to a PFS location, even if it does not exist
    // yet (its parent must). Only \Partitions\<pfs>\... paths are targets.
    [[nodiscard]] Target resolve(std::string_view path);

    [[nodiscard]] MountLookupResult lookup(std::string_view path);
    [[nodiscard]] MountListResult list_directory(std::string_view path);
    [[nodiscard]] MountReadResult read_file(std::string_view path, std::uint64_t offset,
                                            std::span<std::byte> out);

    [[nodiscard]] PfsWriteBack& engine() noexcept { return engine_; }

private:
    ReadOnlyMountView base_;
    PfsWriteBack& engine_;
};

} // namespace ps2hdd
