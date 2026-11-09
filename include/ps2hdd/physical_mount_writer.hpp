#pragma once

#ifdef _WIN32

#include "ps2hdd/pfs_write_back.hpp"

#include <filesystem>
#include <string>

namespace ps2hdd {

struct PhysicalMountWriter {
    bool ok{};
    std::string error;
    std::filesystem::path hddmbr_path;
    WriterFactory factory;
};

// Builds the write-capability factory for a writable mount of a physical PS2
// HDD. Once, at mount time: read-only admission, APA layout fingerprint and an
// FHDB-compatible HDDMBR backup in `artifact_directory`. Per commit batch:
// fresh read-only admission (the identity windows may legitimately contain
// data committed by the previous batch), fingerprint comparison against the
// mounted layout, then a short-lived WritablePhysicalDrive lease with volume
// locks that the engine releases as soon as the batch ends.
[[nodiscard]] PhysicalMountWriter make_physical_mount_writer(unsigned physical_drive_index,
                                                             const std::filesystem::path& artifact_directory);

} // namespace ps2hdd

#endif
