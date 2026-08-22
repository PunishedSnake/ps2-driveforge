#pragma once

#include "ps2hdd/apa_allocation.hpp"
#include "ps2hdd/apa_hdl_headers.hpp"
#include "ps2hdd/write_transaction.hpp"

#include <string>

namespace ps2hdd::apa {

// Stage, but do not commit, the APA metadata changes needed to publish one
// planned HDL allocation. Newly created headers are staged first while still
// unreachable from the old chain; validated prev/next rewrites of existing
// headers are staged afterwards. WriteTransaction owns before-images/rollback.
//
// This function performs metadata reads only. Calling it does not mutate the
// source. A future image installer decides when the transaction may commit.
[[nodiscard]] std::string stage_hdl_header_publication(
    WritableBlockDevice& device,
    const AllocationPlan& allocation,
    const HdlHeaderPlan& new_headers,
    WriteTransaction& transaction);

} // namespace ps2hdd::apa
