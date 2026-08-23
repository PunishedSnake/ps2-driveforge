#include "ps2hdd/drive_session.hpp"

namespace ps2hdd {

bool DriveSession::apply_committed_partition_removal(const apa::RemovePlan& plan)
{
    last_error_.clear();
    if (!apa::apply_remove_plan_to_scan(scan_, plan)) {
        last_error_ = "Committed APA removal plan does not match the cached session snapshot";
        return false;
    }

    // The source mutation was committed by a separate writer. Drop only cached
    // reads/metadata derived from the old on-disk state. Crucially, this does not
    // call scan(): the caller already knows exactly which APA headers changed.
    clear_caches();
    return true;
}

} // namespace ps2hdd
