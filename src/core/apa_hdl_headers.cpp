#include "ps2hdd/apa_hdl_headers.hpp"

#include <algorithm>
#include <cstring>

namespace ps2hdd::apa {

HdlHeaderPlan build_hdl_headers(const AllocationPlan& allocation,
                                const std::string& partition_id,
                                const Ps2Time& created)
{
    HdlHeaderPlan result;
    if (!allocation.ok) {
        result.error = "Cannot serialize HDL APA headers from an invalid allocation plan";
        return result;
    }
    if (allocation.extents.empty()) {
        result.error = "HDL allocation plan contains no extents";
        return result;
    }
    if (allocation.extents.size() > kMaxSub + 1ULL) {
        result.error = "HDL allocation exceeds the 1 main + 64 sub-partition limit";
        return result;
    }
    if (partition_id.empty() || partition_id.size() > kIdMax) {
        result.error = "APA HDL partition ID must contain 1..32 bytes";
        return result;
    }

    const auto main_lba = allocation.extents.front().start_lba;
    result.headers.resize(allocation.extents.size());

    for (std::size_t index = 0; index < allocation.extents.size(); ++index) {
        const auto& extent = allocation.extents[index];
        auto& header = result.headers[index];
        if (extent.length_sectors == 0 || extent.start_lba % kAllocationChunkSectors != 0 ||
            extent.length_sectors % kAllocationChunkSectors != 0) {
            result.headers.clear();
            result.error = "Planned HDL extent is not a valid 128 MiB APA allocation";
            return result;
        }

        header.magic = kMagic;
        header.next = extent.next_lba;
        header.prev = extent.prev_lba;
        header.start = extent.start_lba;
        header.length = extent.length_sectors;
        header.type = kTypeHdl;
        header.created = created;
        header.modver = 0x201;

        if (index == 0) {
            std::memcpy(header.id, partition_id.data(), partition_id.size());
            header.flags = 0;
            header.nsub = static_cast<std::uint32_t>(allocation.extents.size() - 1);
            header.main = 0;
            header.number = 0;
            for (std::size_t sub = 1; sub < allocation.extents.size(); ++sub) {
                header.subs[sub - 1].start = allocation.extents[sub].start_lba;
                header.subs[sub - 1].length = allocation.extents[sub].length_sectors;
            }
        } else {
            header.flags = kFlagSub;
            header.nsub = 0;
            header.main = main_lba;
            header.number = static_cast<std::uint32_t>(index);
        }
        header.checksum = checksum(header);
    }

    result.ok = true;
    return result;
}

} // namespace ps2hdd::apa
