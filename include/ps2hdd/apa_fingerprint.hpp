#pragma once

#include "ps2hdd/apa.hpp"

#include <cstdint>
#include <string>

namespace ps2hdd {

// Stable identity of an APA layout that PFS writes can never change: device
// size plus every main/sub header's id, type, start and length in chain
// order. Used by long-lived writable mounts to prove, before every commit
// batch, that the target is still the disk that was mounted (Windows can
// renumber PhysicalDrive indexes at any time) and that nobody repartitioned
// it underneath the mount.
[[nodiscard]] inline std::string apa_layout_fingerprint(const apa::ScanResult& scan,
                                                        std::uint64_t device_size)
{
    std::string fingerprint = std::to_string(device_size);
    for (const auto& partition : scan.partitions) {
        fingerprint += '|';
        fingerprint += partition.is_sub() ? "s:" : "m:";
        fingerprint += partition.id;
        fingerprint += ':' + std::to_string(partition.type) + ':' + std::to_string(partition.start_lba) + ':' +
                       std::to_string(partition.length_sectors);
    }
    return fingerprint;
}

} // namespace ps2hdd
