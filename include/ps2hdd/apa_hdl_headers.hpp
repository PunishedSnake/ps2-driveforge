#pragma once

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_allocation.hpp"

#include <string>
#include <vector>

namespace ps2hdd::apa {

struct HdlHeaderPlan {
    bool ok{};
    std::string error;
    std::vector<Header> headers;
};

// Convert a validated zero-write AllocationPlan into the exact new APA main/sub
// headers that would describe one HDL game. This is still a pure operation: it
// neither reads nor writes a device and does not patch existing neighbours.
// Existing prev/next rewrites remain separate LinkUpdate entries in the source
// AllocationPlan so publication can later be staged transactionally.
[[nodiscard]] HdlHeaderPlan build_hdl_headers(const AllocationPlan& allocation,
                                              const std::string& partition_id,
                                              const Ps2Time& created);

} // namespace ps2hdd::apa
