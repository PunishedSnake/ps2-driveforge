#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/physical_discovery.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/version.hpp"
#include "windows_theme.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ps2driveforge::gui::ThemePreference;

constexpr wchar_t kWindowClass[] = L"PS2DriveForgeMainWindow";
constexpr UINT kIdOpenImage = 100;
constexpr UINT kIdExit = 101;
constexpr UINT kIdAbout = 102;
constexpr UINT kIdDetectPhysical = 103;
constexpr UINT kIdThemeSystem = 104;
constexpr UINT kIdThemeLight = 105;
constexpr UINT kIdThemeDark = 106;
constexpr UINT kIdPhysicalBase = 200;
constexpr UINT kPhysicalCount = 32;
constexpr int kTreeId = 1000;
constexpr int kListId = 1001;
constexpr int kStatusId = 1002;

std::wstring widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
    if (count <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::wstring format_bytes(std::uint64_t bytes)
{
    constexpr const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::wostringstream out;
    out.setf(std::ios::fixed);
    out.precision(unit == 0 ? 0 : 2);
    out << value << L' ' << units[unit];
    return out.str();
}

std::string join_pfs_path(std::string_view parent, std::string_view child)
{
    if (parent.empty()) {
        return std::string(child);
    }
    std::string result(parent);
    result.push_back('/');
    result.append(child);
    return result;
}

std::string parent_pfs_path(std::string_view path)
{
    const auto pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? std::string{} : std::string(path.substr(0, pos));
}

UINT theme_command(ThemePreference preference)
{
    switch (preference) {
    case ThemePreference::Light:
        return kIdThemeLight;
    case ThemePreference::Dark:
        return kIdThemeDark;
    case ThemePreference::System:
    default:
        return kIdThemeSystem;
    }
}

class App {
public:
    explicit App(HWND window)
        : window_(window), theme_preference_(ps2driveforge::gui::load_theme_preference())
    {
    }

    ~App()
    {
        if (background_brush_) {
            DeleteObject(background_brush_);
        }
    }

    void create_controls()
    {
        tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES |
                                    TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                                0, 0, 0, 0, window_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTreeId)),
                                GetModuleHandleW(nullptr), nullptr);
        list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                    LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                                0, 0, 0, 0, window_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
                                GetModuleHandleW(nullptr), nullptr);
        status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"No PS2 HDD opened — READ ONLY",
                                  WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, window_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)),
                                  GetModuleHandleW(nullptr), nullptr);

        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                    LVS_EX_LABELTIP | LVS_EX_GRIDLINES);

        add_column(0, L"Name", 280);
        add_column(1, L"Type", 90);
        add_column(2, L"Size", 110);
        add_column(3, L"Sub", 70);
        add_column(4, L"Inode", 100);

        apply_theme(false);
    }

    void resize(int width, int height)
    {
        SendMessageW(status_, WM_SIZE, 0, 0);
        RECT status_rect{};
        GetWindowRect(status_, &status_rect);
        const int status_height = status_rect.bottom - status_rect.top;
        const int content_height = std::max(0, height - status_height);
        const int tree_width = std::clamp(width / 3, 230, 420);
        MoveWindow(tree_, 0, 0, tree_width, content_height, TRUE);
        MoveWindow(list_, tree_width, 0, std::max(0, width - tree_width), content_height, TRUE);
    }

    void erase_background(HDC dc) const
    {
        if (!dc || !background_brush_) {
            return;
        }
        RECT rect{};
        GetClientRect(window_, &rect);
        FillRect(dc, &rect, background_brush_);
    }

    void system_settings_changed()
    {
        // WM_SETTINGCHANGE covers Windows app-theme and High Contrast changes.
        // Re-resolve even for forced Light/Dark so accessibility always wins.
        apply_theme(false);
    }

    void set_theme(ThemePreference preference)
    {
        if (theme_preference_ == preference) {
            return;
        }
        theme_preference_ = preference;
        ps2driveforge::gui::save_theme_preference(preference);
        apply_theme(true);
    }

    bool open_image_dialog()
    {
        std::array<wchar_t, 32768> path{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"Disk images (*.img;*.bin;*.raw)\0*.img;*.bin;*.raw\0All files\0*.*\0";
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (!GetOpenFileNameW(&dialog)) {
            return false;
        }

        auto source = std::make_unique<ps2hdd::FileBlockDevice>(std::filesystem::path(path.data()));
        if (!source->is_open()) {
            message(L"Could not open the selected disk image.", MB_ICONERROR);
            return false;
        }
        return load_device(std::move(source));
    }

    bool open_physical(unsigned index)
    {
        auto source = std::make_unique<ps2hdd::PhysicalDrive>(index);
        if (!source->is_open()) {
            std::wostringstream text;
            text << L"Could not open \\\\.\\PhysicalDrive" << index
                 << L" for read-only access.\n\nRun DriveForge as Administrator if required.";
            message(text.str(), MB_ICONERROR);
            return false;
        }
        return load_device(std::move(source));
    }

    void detect_physical()
    {
        const auto probes = ps2hdd::discover_physical_drives(kPhysicalCount);
        std::wostringstream text;
        std::size_t candidates = 0;
        for (const auto& probe : probes) {
            if (!probe.apa_detected) {
                continue;
            }
            ++candidates;
            text << L"PhysicalDrive" << probe.index << L" — " << format_bytes(probe.size_bytes)
                 << L" — APA v" << probe.apa_version << L" — " << probe.partition_count << L" headers";
            if (!probe.apa_clean) {
                text << L" — DIAGNOSTIC ERRORS";
            }
            text << L'\n';
        }

        if (candidates == 0) {
            text << L"No PS2 APA disk was detected among openable PhysicalDrive0.."
                 << (kPhysicalCount - 1) << L".\n\n"
                 << L"If the expected disk is missing, run DriveForge as Administrator and verify that Windows can see the device.";
        } else {
            text << L"\nDetected " << candidates << L" PS2 APA candidate"
                 << (candidates == 1 ? L"." : L"s.")
                 << L"\n\nOpen the desired disk from File -> Open physical drive.\n"
                 << L"Discovery and opening both request GENERIC_READ only.";
        }
        message(text.str(), MB_ICONINFORMATION);
    }

    void tree_selection_changed(const NMTREEVIEWW& notification)
    {
        const auto data = static_cast<std::size_t>(notification.itemNew.lParam);
        if (data == 0) {
            show_drive_overview();
            return;
        }
        const std::size_t index = data - 1;
        if (index < main_partitions_.size()) {
            open_partition(main_partitions_[index]);
        }
    }

    void list_double_click()
    {
        if (!session_ || !active_partition_) {
            return;
        }
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0) {
            return;
        }
        if (!active_path_.empty() && selected == 0) {
            navigate(parent_pfs_path(active_path_));
            return;
        }

        const std::size_t offset = active_path_.empty() ? 0U : 1U;
        const std::size_t entry_index = static_cast<std::size_t>(selected) - offset;
        if (entry_index >= visible_entries_.size()) {
            return;
        }
        const auto& entry = visible_entries_[entry_index];
        if (entry.is_directory()) {
            navigate(join_pfs_path(active_path_, entry.name));
        } else if (entry.is_regular()) {
            extract_entry(entry);
        }
    }

    void about()
    {
        std::wostringstream text;
        text << L"PS2 DriveForge " << widen(ps2hdd::version::string) << L"-dev ("
             << widen(ps2hdd::version::codename) << L")\n\n"
             << L"Native read-only APA/PFS browser for PlayStation 2 HDDs.\n\n"
             << L"The GUI and CLI share the same DriveSession/PFS reader/export path.\n"
             << L"Physical drives are opened with GENERIC_READ only.\n"
             << L"Theme: System / Light / Dark with High Contrast passthrough.";
        message(text.str(), MB_ICONINFORMATION);
    }

private:
    void update_theme_menu() const
    {
        HMENU menu = GetMenu(window_);
        if (!menu) {
            return;
        }
        HMENU view = GetSubMenu(menu, 1);
        HMENU theme = view ? GetSubMenu(view, 0) : nullptr;
        if (!theme) {
            return;
        }
        CheckMenuRadioItem(theme, kIdThemeSystem, kIdThemeDark,
                           theme_command(theme_preference_), MF_BYCOMMAND);
    }

    void apply_theme(bool process_mode_changed)
    {
        if (process_mode_changed) {
            ps2driveforge::gui::apply_process_theme(theme_preference_);
        }

        const auto palette = ps2driveforge::gui::palette_for(theme_preference_);
        if (background_brush_) {
            DeleteObject(background_brush_);
        }
        background_brush_ = CreateSolidBrush(palette.window_background);

        ps2driveforge::gui::apply_window_theme(window_, tree_, list_, status_, theme_preference_);
        update_theme_menu();
        InvalidateRect(window_, nullptr, TRUE);
    }

    void add_column(int index, const wchar_t* title, int width)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(title);
        column.cx = width;
        column.iSubItem = index;
        ListView_InsertColumn(list_, index, &column);
    }

    void message(std::wstring_view text, UINT flags) const
    {
        MessageBoxW(window_, std::wstring(text).c_str(), L"PS2 DriveForge", MB_OK | flags);
    }

    void set_status(std::wstring_view text)
    {
        const std::wstring copy(text);
        SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(copy.c_str()));
    }

    std::wstring io_suffix() const
    {
        if (!session_) {
            return {};
        }
        const auto io = session_->stats().backing_io;
        std::wostringstream out;
        out << L"  |  reads " << io.read_calls << L" / " << format_bytes(io.bytes_requested);
        return out.str();
    }

    bool load_device(std::unique_ptr<ps2hdd::BlockDevice> source)
    {
        auto candidate = std::make_unique<ps2hdd::DriveSession>(std::move(source));
        const bool clean = candidate->scan();
        if (!candidate->scan_result().mbr_valid) {
            message(L"The selected source does not contain a valid PS2 APA MBR.", MB_ICONERROR);
            return false;
        }
        if (!clean) {
            const int answer = MessageBoxW(window_,
                                           L"APA diagnostics contain fatal errors. DriveForge will keep the source read-only.\n\nOpen it anyway for inspection?",
                                           L"PS2 DriveForge", MB_YESNO | MB_ICONWARNING);
            if (answer != IDYES) {
                return false;
            }
        }

        session_ = std::move(candidate);
        active_path_.clear();
        active_partition_.reset();
        visible_entries_.clear();
        populate_tree();
        show_drive_overview();
        return true;
    }

    void populate_tree()
    {
        TreeView_DeleteAllItems(tree_);
        main_partitions_.clear();
        if (!session_) {
            return;
        }

        TVINSERTSTRUCTW root_insert{};
        root_insert.hParent = TVI_ROOT;
        root_insert.hInsertAfter = TVI_LAST;
        root_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        std::wstring root_text = widen(session_->device().display_name());
        root_insert.item.pszText = root_text.data();
        root_insert.item.lParam = 0;
        const HTREEITEM root = TreeView_InsertItem(tree_, &root_insert);

        for (const auto& partition : session_->scan_result().partitions) {
            if (partition.is_sub()) {
                continue;
            }
            main_partitions_.push_back(partition);
            const auto index = main_partitions_.size() - 1;
            std::wstring text = widen(partition.id.empty() ? "<unnamed>" : partition.id);
            text += L"  [" + widen(ps2hdd::apa::type_name(partition.type)) + L"]";

            TVINSERTSTRUCTW insert{};
            insert.hParent = root;
            insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT | TVIF_PARAM;
            insert.item.pszText = text.data();
            insert.item.lParam = static_cast<LPARAM>(index + 1);
            TreeView_InsertItem(tree_, &insert);
        }
        TreeView_Expand(tree_, root, TVE_EXPAND);
        TreeView_SelectItem(tree_, root);
    }

    void show_drive_overview()
    {
        active_partition_.reset();
        active_path_.clear();
        visible_entries_.clear();
        ListView_DeleteAllItems(list_);
        if (!session_) {
            set_status(L"No PS2 HDD opened — READ ONLY");
            return;
        }

        const auto& scan = session_->scan_result();
        insert_list_row(0, L"PS2 APA HDD", L"Drive", format_bytes(session_->device().size_bytes()), L"", L"");
        insert_list_row(1, L"APA version", L"Metadata", std::to_wstring(scan.apa_version), L"", L"");
        insert_list_row(2, L"Main partitions", L"Metadata", std::to_wstring(main_partitions_.size()), L"", L"");
        insert_list_row(3, L"Diagnostics", L"Metadata", scan.ok() ? L"clean" : L"errors", L"", L"");

        std::wostringstream status;
        status << widen(session_->device().display_name()) << L"  |  APA v" << scan.apa_version
               << L"  |  " << main_partitions_.size() << L" main partitions  |  READ ONLY"
               << io_suffix();
        set_status(status.str());
    }

    void open_partition(const ps2hdd::apa::Partition& partition)
    {
        active_partition_ = partition;
        active_path_.clear();
        visible_entries_.clear();
        ListView_DeleteAllItems(list_);

        if (partition.type != ps2hdd::apa::kTypePfs) {
            insert_list_row(0, widen(partition.id), widen(ps2hdd::apa::type_name(partition.type)),
                            format_bytes(partition.size_bytes()), L"", L"");
            set_status(widen(partition.id) + L"  |  Not a PFS filesystem  |  READ ONLY" + io_suffix());
            return;
        }
        navigate("");
    }

    void navigate(std::string path)
    {
        if (!session_ || !active_partition_) {
            return;
        }
        const auto result = session_->browse(active_partition_->id, path);
        if (!result.ok) {
            message(L"Could not enumerate PFS directory:\n" + widen(result.error), MB_ICONERROR);
            return;
        }

        active_path_ = std::move(path);
        visible_entries_ = result.entries;
        std::sort(visible_entries_.begin(), visible_entries_.end(), [](const auto& a, const auto& b) {
            if (a.is_directory() != b.is_directory()) {
                return a.is_directory() > b.is_directory();
            }
            return a.name < b.name;
        });

        ListView_DeleteAllItems(list_);
        int row = 0;
        if (!active_path_.empty()) {
            insert_list_row(row++, L"..", L"Folder", L"", L"", L"");
        }
        for (const auto& entry : visible_entries_) {
            const std::wstring type = entry.is_directory() ? L"Folder" : (entry.is_regular() ? L"File" : L"Other");
            insert_list_row(row++, widen(entry.name), type,
                            entry.inode_readable ? format_bytes(entry.size) : L"?",
                            std::to_wstring(entry.inode.subpart),
                            std::to_wstring(entry.inode.number));
        }

        std::wstring location = widen(active_partition_->id) + L":/" + widen(active_path_);
        std::wostringstream status;
        status << location << L"  |  " << visible_entries_.size() << L" entries  |  READ ONLY"
               << io_suffix() << L"  |  Double-click a folder to open, a file to extract";
        set_status(status.str());
    }

    void insert_list_row(int row, const std::wstring& name, const std::wstring& type,
                         const std::wstring& size, const std::wstring& sub,
                         const std::wstring& inode)
    {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        ListView_InsertItem(list_, &item);
        ListView_SetItemText(list_, row, 1, const_cast<wchar_t*>(type.c_str()));
        ListView_SetItemText(list_, row, 2, const_cast<wchar_t*>(size.c_str()));
        ListView_SetItemText(list_, row, 3, const_cast<wchar_t*>(sub.c_str()));
        ListView_SetItemText(list_, row, 4, const_cast<wchar_t*>(inode.c_str()));
    }

    void extract_entry(const ps2hdd::SessionEntry& entry)
    {
        if (!session_ || !active_partition_ || !entry.is_regular()) {
            return;
        }

        std::array<wchar_t, 32768> output{};
        const auto default_name = widen(entry.name);
        std::copy_n(default_name.c_str(), std::min(default_name.size(), output.size() - 1), output.data());
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFile = output.data();
        dialog.nMaxFile = static_cast<DWORD>(output.size());
        dialog.lpstrFilter = L"All files\0*.*\0";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (!GetSaveFileNameW(&dialog)) {
            return;
        }

        const auto source_path = join_pfs_path(active_path_, entry.name);
        const auto result = session_->export_to_host(active_partition_->id, source_path,
                                                     std::filesystem::path(output.data()));
        if (!result.ok) {
            message(L"Extraction failed:\n\n" + widen(result.error), MB_ICONERROR);
            return;
        }

        std::wostringstream text;
        text << L"Extracted " << result.stats.files << L" file, " << format_bytes(result.stats.bytes)
             << L" to:\n" << output.data() << L"\n\n"
             << L"Backing I/O so far: " << session_->stats().backing_io.read_calls << L" reads / "
             << format_bytes(session_->stats().backing_io.bytes_requested);
        message(text.str(), MB_ICONINFORMATION);
        navigate(active_path_);
    }

    HWND window_{};
    HWND tree_{};
    HWND list_{};
    HWND status_{};
    HBRUSH background_brush_{};
    ThemePreference theme_preference_{ThemePreference::System};
    std::unique_ptr<ps2hdd::DriveSession> session_;
    std::vector<ps2hdd::apa::Partition> main_partitions_;
    std::optional<ps2hdd::apa::Partition> active_partition_;
    std::string active_path_;
    std::vector<ps2hdd::SessionEntry> visible_entries_;
};

HMENU create_menu()
{
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU physical = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kIdOpenImage, L"Open disk &image...");
    AppendMenuW(file, MF_STRING, kIdDetectPhysical, L"&Detect PS2 HDDs...");
    for (UINT i = 0; i < kPhysicalCount; ++i) {
        std::wstring label = L"PhysicalDrive" + std::to_wstring(i);
        AppendMenuW(physical, MF_STRING, kIdPhysicalBase + i, label.c_str());
    }
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(physical), L"Open &physical drive");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kIdExit, L"E&xit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

    HMENU view = CreatePopupMenu();
    HMENU theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING, kIdThemeSystem, L"&System");
    AppendMenuW(theme, MF_STRING, kIdThemeLight, L"&Light");
    AppendMenuW(theme, MF_STRING, kIdThemeDark, L"&Dark");
    CheckMenuRadioItem(theme, kIdThemeSystem, kIdThemeDark, kIdThemeSystem, MF_BYCOMMAND);
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), L"&Theme");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, kIdAbout, L"&About PS2 DriveForge");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return menu;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        auto owned = std::make_unique<App>(window);
        owned->create_controls();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned.release()));
        return 0;
    }
    case WM_ERASEBKGND:
        if (app) {
            app->erase_background(reinterpret_cast<HDC>(wparam));
            return 1;
        }
        break;
    case WM_SIZE:
        if (app) {
            app->resize(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (app) {
            app->system_settings_changed();
        }
        return 0;
    case WM_COMMAND:
        if (!app) {
            break;
        }
        switch (LOWORD(wparam)) {
        case kIdOpenImage:
            app->open_image_dialog();
            return 0;
        case kIdDetectPhysical:
            app->detect_physical();
            return 0;
        case kIdThemeSystem:
            app->set_theme(ThemePreference::System);
            return 0;
        case kIdThemeLight:
            app->set_theme(ThemePreference::Light);
            return 0;
        case kIdThemeDark:
            app->set_theme(ThemePreference::Dark);
            return 0;
        case kIdExit:
            DestroyWindow(window);
            return 0;
        case kIdAbout:
            app->about();
            return 0;
        default:
            break;
        }
        if (LOWORD(wparam) >= kIdPhysicalBase && LOWORD(wparam) < kIdPhysicalBase + kPhysicalCount) {
            app->open_physical(LOWORD(wparam) - kIdPhysicalBase);
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (!app) {
            break;
        }
        if (reinterpret_cast<NMHDR*>(lparam)->idFrom == kTreeId &&
            reinterpret_cast<NMHDR*>(lparam)->code == TVN_SELCHANGEDW) {
            app->tree_selection_changed(*reinterpret_cast<NMTREEVIEWW*>(lparam));
            return 0;
        }
        if (reinterpret_cast<NMHDR*>(lparam)->idFrom == kListId &&
            reinterpret_cast<NMHDR*>(lparam)->code == NM_DBLCLK) {
            app->list_double_click();
            return 0;
        }
        break;
    case WM_DESTROY:
        delete app;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    INITCOMMONCONTROLSEX common{};
    common.dwSize = sizeof(common);
    common.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&common);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const auto initial_theme = ps2driveforge::gui::load_theme_preference();
    ps2driveforge::gui::apply_process_theme(initial_theme);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    // The client background is painted in WM_ERASEBKGND from the active palette;
    // leaving the class brush null prevents a white flash before a dark resize.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    std::wstring title = L"PS2 DriveForge " + widen(ps2hdd::version::string) + L"-dev — " +
                         widen(ps2hdd::version::codename);
    HWND window = CreateWindowExW(0, kWindowClass, title.c_str(),
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1120, 720,
                                  nullptr, create_menu(), instance, nullptr);
    if (!window) {
        return 1;
    }
    ShowWindow(window, show);
    UpdateWindow(window);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
