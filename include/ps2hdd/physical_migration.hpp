#pragma once

#ifdef _WIN32

#include "ps2hdd/apa_migrate.hpp"

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>

namespace ps2hdd {

struct PhysicalMigrationResult {
    bool ok{};
    std::string error;
    std::size_t locked_volume_count{};
    bool cold_verified{};
    migrate::Result copy;
};

// Accepts only a target that is blank (no MBR signature, no GPT header) or
// already a PS2 APA disk, is not the source drive, and is large enough. A disk
// with any PC partition table is refused outright: clearing it is a decision
// for Disk Management, not something a PS2 tool infers. The caller supplies an
// already-frozen plan from the same source device.
[[nodiscard]] PhysicalMigrationResult migrate_to_physical(
    BlockDevice& source,
    std::optional<unsigned> source_physical_index,
    unsigned target_physical_index,
    const migrate::Plan& plan,
    migrate::Progress progress = {},
    std::stop_token stop = {});

// Read-only check that explains why a target would be refused.
[[nodiscard]] bool check_migration_target(unsigned target_physical_index,
                                          std::optional<unsigned> source_physical_index,
                                          std::uint64_t required_bytes,
                                          std::uint64_t& target_bytes,
                                          std::string& error);

} // namespace ps2hdd

#endif
