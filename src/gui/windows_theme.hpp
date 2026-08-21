#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ps2driveforge::gui {

enum class ThemePreference : unsigned {
    System = 0,
    Light = 1,
    Dark = 2,
};

struct ThemePalette {
    bool dark{};
    COLORREF window_background{};
    COLORREF control_background{};
    COLORREF text{};
    COLORREF muted_text{};
    COLORREF border{};
};

[[nodiscard]] ThemePreference load_theme_preference();
void save_theme_preference(ThemePreference preference);

[[nodiscard]] bool high_contrast_enabled();
[[nodiscard]] bool system_prefers_dark();
[[nodiscard]] ThemePalette palette_for(ThemePreference preference);

// Apply the process-wide menu/common-control preference before creating the
// main window. The helper uses documented APIs where possible and treats the
// optional UxTheme dark-menu entry points as a best-effort enhancement.
void apply_process_theme(ThemePreference preference);

// Apply the resolved theme to the top-level frame and controls. High Contrast
// deliberately wins over DriveForge's own Light/Dark choice.
void apply_window_theme(HWND window, HWND tree, HWND list, HWND status,
                        ThemePreference preference);

} // namespace ps2driveforge::gui
