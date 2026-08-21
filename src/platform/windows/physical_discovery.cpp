#include "ps2hdd/physical_discovery.hpp"

#ifdef _WIN32

#include "ps2hdd/apa.hpp"
#include "ps2hdd/physical_drive.hpp"

#include <string>

namespace ps2hdd {

std::vector<PhysicalDriveProbe> discover_physical_drives(unsigned max_index)
{
    std::vector<PhysicalDriveProbe> result;
    result.reserve(max_index);

    for (unsigned index = 0; index < max_index; ++index) {
        PhysicalDriveProbe probe;
        probe.index = index;

        PhysicalDrive drive(index);
        if (!drive.is_open()) {
            continue;
        }

        probe.opened = true;
        probe.size_bytes = drive.size_bytes();

        apa::Reader reader(drive);
        const auto scan = reader.scan();
        probe.apa_detected = scan.mbr_valid;
        probe.apa_clean = scan.ok();
        probe.apa_version = scan.apa_version;
        probe.partition_count = scan.partitions.size();

        if (!scan.mbr_valid) {
            probe.note = "not a PS2 APA disk";
        } else if (!scan.ok()) {
            probe.note = "APA detected with fatal diagnostics";
        } else {
            probe.note = "PS2 APA disk";
        }
        result.emplace_back(std::move(probe));
    }

    return result;
}

} // namespace ps2hdd

#endif
