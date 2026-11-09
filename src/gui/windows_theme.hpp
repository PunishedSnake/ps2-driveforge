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
    COLORREF menu_bar{};       // dark menu bar background
    COLORREF menu_hot{};       // hot/selected menu bar item
    COLORREF splitter{};       // 1px divider between tree and list
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

// Dark menu bar. Win32 has no documented dark menu *bar* (popups follow the
// process app mode); the undocumented-but-stable UAH paint messages are what
// dark-mode Win32 apps use. Returns true when the message was fully handled.
// Call from the window procedure before DefWindowProc; no-op in light mode.
[[nodiscard]] bool handle_menu_bar_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                           ThemePreference preference, LRESULT& result);

// Message font (Segoe UI on modern Windows) at an explicit DPI. Caller owns it.
[[nodiscard]] HFONT create_ui_font(UINT dpi);

// Window placement + tree width persistence (HKCU\Software\PS2DriveForge).
void save_window_layout(HWND window, int tree_width_dip);
[[nodiscard]] bool restore_window_layout(HWND window, int show, int& tree_width_dip);

} // namespace ps2driveforge::gui
