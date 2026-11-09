#pragma once

#ifdef _WIN32

#include <cstdint>
#include <filesystem>
#include <optional>

namespace ps2hdd {

// Finds the Windows volume backed by the partition that starts at
// `start_offset_bytes` on PhysicalDrive<disk_number> (for APA-Jail: the exFAT
// MBR entry). Returns its first mount path ("E:\") or, if it has no drive
// letter, its "\\?\Volume{GUID}\" path, which works as a filesystem root.
[[nodiscard]] std::optional<std::filesystem::path> find_partition_volume(unsigned disk_number,
                                                                         std::uint64_t start_offset_bytes);

} // namespace ps2hdd

#endif
