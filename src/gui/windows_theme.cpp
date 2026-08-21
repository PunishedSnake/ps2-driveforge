#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "windows_theme.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <cstring>

namespace ps2driveforge::gui {
namespace {

constexpr wchar_t kPreferenceKey[] = L"Software\\PS2DriveForge";
constexpr wchar_t kPreferenceValue[] = L"Theme";
constexpr wchar_t kWindowsPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kAppsUseLightTheme[] = L"AppsUseLightTheme";

// UxTheme does not expose a documented process-wide Win32 dark-menu switch.
// Windows 10/11 nevertheless export these entry points by ordinal and native
// applications commonly use them. Resolve them dynamically so a future Windows
// version can remove/change them without preventing DriveForge from starting;
// our explicit control colours and documented DWM title-bar path remain active.
enum class PreferredAppMode : int {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI*)();
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);

template <typename Function>
Function load_ordinal(HMODULE module, WORD ordinal)
{
    const FARPROC raw = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
    Function function{};
    static_assert(sizeof(function) == sizeof(raw));
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

bool query_dword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD& value)
{
    DWORD size = sizeof(value);
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
}

bool resolved_dark(ThemePreference preference)
{
    if (high_contrast_enabled()) {
        return false;
    }
    if (preference == ThemePreference::Dark) {
        return true;
    }
    if (preference == ThemePreference::Light) {
        return false;
    }
    return system_prefers_dark();
}

void allow_dark_for_window(HWND window, bool dark)
{
    if (!window) {
        return;
    }
    HMODULE theme = LoadLibraryW(L"uxtheme.dll");
    if (!theme) {
        return;
    }
    if (const auto allow = load_ordinal<AllowDarkModeForWindowFn>(theme, 133)) {
        allow(window, dark ? TRUE : FALSE);
    }
    FreeLibrary(theme);
}

} // namespace

ThemePreference load_theme_preference()
{
    DWORD value = static_cast<DWORD>(ThemePreference::System);
    if (!query_dword(HKEY_CURRENT_USER, kPreferenceKey, kPreferenceValue, value)) {
        return ThemePreference::System;
    }
    if (value > static_cast<DWORD>(ThemePreference::Dark)) {
        return ThemePreference::System;
    }
    return static_cast<ThemePreference>(value);
}

void save_theme_preference(ThemePreference preference)
{
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kPreferenceKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD value = static_cast<DWORD>(preference);
    RegSetValueExW(key, kPreferenceValue, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

bool high_contrast_enabled()
{
    HIGHCONTRASTW high_contrast{};
    high_contrast.cbSize = sizeof(high_contrast);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast), &high_contrast, 0)) {
        return false;
    }
    return (high_contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool system_prefers_dark()
{
    DWORD use_light = 1;
    if (!query_dword(HKEY_CURRENT_USER, kWindowsPersonalizeKey, kAppsUseLightTheme, use_light)) {
        return false;
    }
    return use_light == 0;
}

ThemePalette palette_for(ThemePreference preference)
{
    if (high_contrast_enabled()) {
        return ThemePalette{
            .dark = false,
            .window_background = GetSysColor(COLOR_WINDOW),
            .control_background = GetSysColor(COLOR_WINDOW),
            .text = GetSysColor(COLOR_WINDOWTEXT),
            .muted_text = GetSysColor(COLOR_GRAYTEXT),
            .border = GetSysColor(COLOR_WINDOWFRAME),
        };
    }

    if (resolved_dark(preference)) {
        // Neutral near-black surfaces avoid HDR eye-searing white while keeping
        // contrast high enough for filesystem metadata and long browsing sessions.
        return ThemePalette{
            .dark = true,
            .window_background = RGB(24, 24, 24),
            .control_background = RGB(30, 30, 30),
            .text = RGB(238, 238, 238),
            .muted_text = RGB(180, 180, 180),
            .border = RGB(58, 58, 58),
        };
    }

    return ThemePalette{
        .dark = false,
        .window_background = GetSysColor(COLOR_WINDOW),
        .control_background = GetSysColor(COLOR_WINDOW),
        .text = GetSysColor(COLOR_WINDOWTEXT),
        .muted_text = GetSysColor(COLOR_GRAYTEXT),
        .border = GetSysColor(COLOR_WINDOWFRAME),
    };
}

void apply_process_theme(ThemePreference preference)
{
    HMODULE theme = LoadLibraryW(L"uxtheme.dll");
    if (!theme) {
        return;
    }

    const auto set_preferred = load_ordinal<SetPreferredAppModeFn>(theme, 135);
    const auto flush_menus = load_ordinal<FlushMenuThemesFn>(theme, 136);
    if (set_preferred) {
        PreferredAppMode mode = PreferredAppMode::AllowDark;
        if (high_contrast_enabled()) {
            mode = PreferredAppMode::Default;
        } else if (preference == ThemePreference::Dark) {
            mode = PreferredAppMode::ForceDark;
        } else if (preference == ThemePreference::Light) {
            mode = PreferredAppMode::ForceLight;
        }
        set_preferred(mode);
    }
    if (flush_menus) {
        flush_menus();
    }
    FreeLibrary(theme);
}

void apply_window_theme(HWND window, HWND tree, HWND list, HWND status,
                        ThemePreference preference)
{
    const auto palette = palette_for(preference);
    const bool high_contrast = high_contrast_enabled();

    if (window) {
        const BOOL dark_frame = palette.dark ? TRUE : FALSE;
        // Documented on Windows 11: opt the standard non-client frame into dark mode.
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &dark_frame, sizeof(dark_frame));
        allow_dark_for_window(window, palette.dark);
    }

    const wchar_t* theme_name = high_contrast ? nullptr :
        (palette.dark ? L"DarkMode_Explorer" : L"Explorer");

    if (tree) {
        allow_dark_for_window(tree, palette.dark);
        SetWindowTheme(tree, theme_name, nullptr);
        TreeView_SetBkColor(tree, palette.control_background);
        TreeView_SetTextColor(tree, palette.text);
    }

    if (list) {
        allow_dark_for_window(list, palette.dark);
        SetWindowTheme(list, theme_name, nullptr);
        ListView_SetBkColor(list, palette.control_background);
        ListView_SetTextBkColor(list, palette.control_background);
        ListView_SetTextColor(list, palette.text);
        if (HWND header = ListView_GetHeader(list)) {
            allow_dark_for_window(header, palette.dark);
            SetWindowTheme(header, theme_name, nullptr);
        }
    }

    if (status) {
        allow_dark_for_window(status, palette.dark);
        SetWindowTheme(status, theme_name, nullptr);
        SendMessageW(status, SB_SETBKCOLOR, 0,
                     static_cast<LPARAM>(palette.dark ? palette.control_background : CLR_DEFAULT));
    }

    if (window) {
        DrawMenuBar(window);
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

} // namespace ps2driveforge::gui
