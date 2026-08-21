#pragma once

#include <windows.h>

namespace ps2driveforge::gui {

enum class ElevationAttempt {
    already_elevated,
    relaunched,
    cancelled,
    failed,
};

[[nodiscard]] bool is_process_elevated() noexcept;

// Relaunch the current GUI executable through the UAC "runas" verb. The caller
// should terminate the unelevated instance only when `relaunched` is returned.
// `relaunch_marker_present` prevents an accidental elevation loop if Windows
// creates an elevated process that still cannot obtain raw-disk access.
[[nodiscard]] ElevationAttempt relaunch_elevated(bool relaunch_marker_present);

} // namespace ps2driveforge::gui
