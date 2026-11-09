#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "windows_theme.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <cstring>
#include <iterator>
#include <string>

namespace ps2driveforge::gui {
namespace {

constexpr wchar_t kPreferenceKey[] = L"Software\\PS2DriveForge";
constexpr wchar_t kPreferenceValue[] = L"Theme";
constexpr wchar_t kWindowsPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kAppsUseLightTheme[] = L"AppsUseLightTheme";
constexpr UINT_PTR kStatusSubclassId = 0x50533244; // "PS2D"
constexpr UINT_PTR kListSubclassId = 0x5053324C;   // "PS2L"
constexpr wchar_t kPlacementValue[] = L"WindowPlacement";
constexpr wchar_t kTreeWidthValue[] = L"TreeWidth";

// UAH menu bar messages (uxtheme internal, stable since Windows Vista).
constexpr UINT WM_UAHDRAWMENU = 0x0091;
constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;

struct UahMenu {
    HMENU menu;
    HDC dc;
    DWORD flags;
};
union UahMenuItemMetrics {
    struct { DWORD cx; DWORD cy; } bar[2];
    struct { DWORD cx; DWORD cy; } popup[4];
};
struct UahMenuPopupMetrics {
    DWORD cx[4];
    DWORD update_max_widths : 2;
};
struct UahMenuItem {
    int position;
    UahMenuItemMetrics metrics;
    UahMenuPopupMetrics popup;
};
struct UahDrawMenuItem {
    DRAWITEMSTRUCT item;
    UahMenu menu;
    UahMenuItem menu_item;
};

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

struct StatusThemeState {
    bool dark{};
    COLORREF background{};
    COLORREF text{};
};

void paint_dark_status_bar(HWND window, HDC dc, const StatusThemeState& state)
{
    RECT rect{};
    GetClientRect(window, &rect);

    HBRUSH brush = CreateSolidBrush(state.background);
    if (brush) {
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }

    const LRESULT raw_length = SendMessageW(window, SB_GETTEXTLENGTHW, 0, 0);
    const int length = LOWORD(raw_length);
    if (length <= 0) {
        return;
    }

    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    SendMessageW(window, SB_GETTEXTW, 0, reinterpret_cast<LPARAM>(text.data()));
    text.resize(std::wcslen(text.c_str()));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, state.text);

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    HGDIOBJ old_font = nullptr;
    if (font) {
        old_font = SelectObject(dc, font);
    }

    // Leave a little room for the standard resize grip at the right edge. The
    // native status control keeps handling sizing/layout; only the dark client
    // paint is replaced so its text cannot fall back to COLOR_WINDOWTEXT black.
    rect.left += 6;
    rect.right = (rect.right > 18) ? rect.right - 18 : rect.right;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    if (old_font) {
        SelectObject(dc, old_font);
    }
}

LRESULT CALLBACK status_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                      UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    auto* state = reinterpret_cast<StatusThemeState*>(ref_data);

    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, status_subclass_proc, subclass_id);
        delete state;
        return DefSubclassProc(window, message, wparam, lparam);
    }

    if (state && state->dark) {
        if (message == WM_ERASEBKGND) {
            return 1;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            if (dc) {
                paint_dark_status_bar(window, dc, *state);
            }
            EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_PRINTCLIENT) {
            HDC dc = reinterpret_cast<HDC>(wparam);
            if (dc) {
                paint_dark_status_bar(window, dc, *state);
                return 0;
            }
        }
    }

    return DefSubclassProc(window, message, wparam, lparam);
}

struct ListThemeState {
    bool dark{};
    COLORREF header_text{};
};

// The header's NM_CUSTOMDRAW goes to its parent (the ListView), not to the
// main window, so dark header text needs a ListView subclass.
LRESULT CALLBACK list_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                    UINT_PTR subclass_id, DWORD_PTR ref_data)
{
    auto* state = reinterpret_cast<ListThemeState*>(ref_data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, list_subclass_proc, subclass_id);
        delete state;
        return DefSubclassProc(window, message, wparam, lparam);
    }
    if (message == WM_NOTIFY && state && state->dark) {
        auto* header = reinterpret_cast<NMHDR*>(lparam);
        if (header->hwndFrom == ListView_GetHeader(window) && header->code == NM_CUSTOMDRAW) {
            auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lparam);
            if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(draw->hdc, state->header_text);
                return CDRF_DODEFAULT;
            }
        }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

ListThemeState* ensure_list_theme_state(HWND list)
{
    DWORD_PTR ref_data = 0;
    if (GetWindowSubclass(list, list_subclass_proc, kListSubclassId, &ref_data))
        return reinterpret_cast<ListThemeState*>(ref_data);
    auto* state = new ListThemeState{};
    if (!SetWindowSubclass(list, list_subclass_proc, kListSubclassId, reinterpret_cast<DWORD_PTR>(state))) {
        delete state;
        return nullptr;
    }
    return state;
}

void paint_menu_bar_underline(HWND window, COLORREF color)
{
    MENUBARINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMenuBarInfo(window, OBJID_MENU, 0, &info)) return;
    RECT client{};
    GetClientRect(window, &client);
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
    RECT frame{};
    GetWindowRect(window, &frame);
    OffsetRect(&client, -frame.left, -frame.top);
    // The stray light line sits in the single row between the bar and the client area.
    RECT line = client;
    line.bottom = line.top;
    line.top = line.top - 1;
    HDC dc = GetWindowDC(window);
    if (!dc) return;
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &line, brush);
    DeleteObject(brush);
    ReleaseDC(window, dc);
}

StatusThemeState* ensure_status_theme_state(HWND status)
{
    DWORD_PTR ref_data = 0;
    if (GetWindowSubclass(status, status_subclass_proc, kStatusSubclassId, &ref_data)) {
        return reinterpret_cast<StatusThemeState*>(ref_data);
    }

    auto* state = new StatusThemeState{};
    if (!SetWindowSubclass(status, status_subclass_proc, kStatusSubclassId,
                           reinterpret_cast<DWORD_PTR>(state))) {
        delete state;
        return nullptr;
    }
    return state;
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
            .menu_bar = GetSysColor(COLOR_MENUBAR),
            .menu_hot = GetSysColor(COLOR_HIGHLIGHT),
            .splitter = GetSysColor(COLOR_WINDOWFRAME),
        };
    }

    if (resolved_dark(preference)) {
        // Neutral near-black surfaces avoid HDR eye-searing white while keeping
        // contrast high enough for filesystem metadata and long browsing sessions.
        return ThemePalette{
            .dark = true,
            .window_background = RGB(32, 32, 32),
            .control_background = RGB(25, 25, 25),
            .text = RGB(240, 240, 240),
            .muted_text = RGB(165, 165, 165),
            .border = RGB(51, 51, 51),
            .menu_bar = RGB(32, 32, 32),
            .menu_hot = RGB(61, 61, 61),
            .splitter = RGB(51, 51, 51),
        };
    }

    // Light: a soft frame around white content panes, like Explorer on
    // Windows 11, instead of one flat white slab.
    return ThemePalette{
        .dark = false,
        .window_background = RGB(243, 243, 243),
        .control_background = RGB(255, 255, 255),
        .text = RGB(26, 26, 26),
        .muted_text = RGB(96, 96, 96),
        .border = RGB(229, 229, 229),
        .menu_bar = GetSysColor(COLOR_MENUBAR),
        .menu_hot = GetSysColor(COLOR_HIGHLIGHT),
        .splitter = RGB(218, 218, 218),
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
            // ItemsView is the dark header visual; Explorer leaves it light.
            SetWindowTheme(header, high_contrast ? nullptr : (palette.dark ? L"DarkMode_ItemsView" : L"ItemsView"),
                           nullptr);
            InvalidateRect(header, nullptr, TRUE);
        }
        if (auto* state = ensure_list_theme_state(list)) {
            state->dark = palette.dark && !high_contrast;
            state->header_text = palette.text;
        }
    }

    if (status) {
        allow_dark_for_window(status, palette.dark);
        SetWindowTheme(status, theme_name, nullptr);
        SendMessageW(status, SB_SETBKCOLOR, 0,
                     static_cast<LPARAM>(palette.dark ? palette.control_background : CLR_DEFAULT));

        if (auto* state = ensure_status_theme_state(status)) {
            state->dark = palette.dark && !high_contrast;
            state->background = palette.control_background;
            state->text = palette.text;
        }
        InvalidateRect(status, nullptr, TRUE);
    }

    if (window) {
        DrawMenuBar(window);
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

bool handle_menu_bar_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                             ThemePreference preference, LRESULT& result)
{
    if (message != WM_UAHDRAWMENU && message != WM_UAHDRAWMENUITEM && message != WM_NCPAINT &&
        message != WM_NCACTIVATE) {
        return false;
    }
    const auto palette = palette_for(preference);
    if (!palette.dark || high_contrast_enabled()) return false;

    if (message == WM_UAHDRAWMENU) {
        auto* menu = reinterpret_cast<UahMenu*>(lparam);
        MENUBARINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMenuBarInfo(window, OBJID_MENU, 0, &info)) return false;
        RECT frame{};
        GetWindowRect(window, &frame);
        RECT bar = info.rcBar;
        OffsetRect(&bar, -frame.left, -frame.top);
        bar.top -= 1;  // cover the 1px seam above the bar on Windows 11
        HBRUSH brush = CreateSolidBrush(palette.menu_bar);
        FillRect(menu->dc, &bar, brush);
        DeleteObject(brush);
        result = 0;
        return true;
    }
    if (message == WM_UAHDRAWMENUITEM) {
        auto* draw = reinterpret_cast<UahDrawMenuItem*>(lparam);
        wchar_t text[256]{};
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_STRING;
        item.dwTypeData = text;
        item.cch = static_cast<UINT>(std::size(text) - 1);
        GetMenuItemInfoW(draw->menu.menu, static_cast<UINT>(draw->menu_item.position), TRUE, &item);

        const UINT state = draw->item.itemState;
        const bool hot = (state & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
        const bool disabled = (state & (ODS_INACTIVE | ODS_DISABLED | ODS_GRAYED)) != 0;
        HBRUSH brush = CreateSolidBrush(hot ? palette.menu_hot : palette.menu_bar);
        FillRect(draw->menu.dc, &draw->item.rcItem, brush);
        DeleteObject(brush);
        SetBkMode(draw->menu.dc, TRANSPARENT);
        SetTextColor(draw->menu.dc, disabled ? palette.muted_text : palette.text);
        UINT format = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        if (state & ODS_NOACCEL) format |= DT_HIDEPREFIX;
        RECT rect = draw->item.rcItem;
        DrawTextW(draw->menu.dc, text, -1, &rect, format);
        result = 0;
        return true;
    }
    // WM_NCPAINT / WM_NCACTIVATE: let Windows paint the frame, then cover the
    // light line it draws between the menu bar and the client area.
    result = DefWindowProcW(window, message, wparam, lparam);
    paint_menu_bar_underline(window, palette.menu_bar);
    return true;
}

HFONT create_ui_font(UINT dpi)
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        metrics.lfMessageFont.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);
        wcscpy_s(metrics.lfMessageFont.lfFaceName, L"Segoe UI");
    }
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

void save_window_layout(HWND window, int tree_width_dip)
{
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(window, &placement)) return;
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kPreferenceKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, kPlacementValue, 0, REG_BINARY, reinterpret_cast<const BYTE*>(&placement), sizeof(placement));
    const DWORD width = static_cast<DWORD>(tree_width_dip);
    RegSetValueExW(key, kTreeWidthValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&width), sizeof(width));
    RegCloseKey(key);
}

bool restore_window_layout(HWND window, int show, int& tree_width_dip)
{
    DWORD width = 0;
    if (query_dword(HKEY_CURRENT_USER, kPreferenceKey, kTreeWidthValue, width) && width >= 160 && width <= 900)
        tree_width_dip = static_cast<int>(width);
    WINDOWPLACEMENT placement{};
    DWORD size = sizeof(placement);
    if (RegGetValueW(HKEY_CURRENT_USER, kPreferenceKey, kPlacementValue, RRF_RT_REG_BINARY, nullptr, &placement,
                     &size) != ERROR_SUCCESS ||
        size != sizeof(placement) || placement.length != sizeof(placement)) {
        return false;
    }
    // Never restore onto a monitor that is gone (undocked laptop, TV off).
    if (!MonitorFromRect(&placement.rcNormalPosition, MONITOR_DEFAULTTONULL)) return false;
    if (placement.showCmd == SW_SHOWMINIMIZED || placement.showCmd == SW_MINIMIZE) placement.showCmd = SW_SHOWNORMAL;
    if (show == SW_SHOWMINNOACTIVE || show == SW_MINIMIZE || show == SW_SHOWMINIMIZED) placement.showCmd = static_cast<UINT>(show);
    placement.flags = 0;
    return SetWindowPlacement(window, &placement) != FALSE;
}

} // namespace ps2driveforge::gui
