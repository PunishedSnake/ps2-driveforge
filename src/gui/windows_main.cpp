#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ps2hdd/apa.hpp"
#include "ps2hdd/apa_volume.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/pfs.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/version.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"PS2DriveForgeMainWindow";
constexpr UINT kIdOpenImage = 100;
constexpr UINT kIdExit = 101;
constexpr UINT kIdAbout = 102;
constexpr UINT kIdPhysicalBase = 200;
constexpr UINT kPhysicalCount = 16;
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

struct VisibleEntry {
    ps2hdd::pfs::DirectoryEntry entry;
    std::uint64_t size{};
    bool inode_readable{};
    bool directory{};
    bool regular{};
};

class App {
public:
    explicit App(HWND window) : window_(window) {}

    void create_controls()
    {
        tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES |
                                    TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kTreeId),
                                GetModuleHandleW(nullptr), nullptr);
        list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                    LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kListId),
                                GetModuleHandleW(nullptr), nullptr);
        status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"No PS2 HDD opened",
                                  WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(kStatusId),
                                  GetModuleHandleW(nullptr), nullptr);

        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                    LVS_EX_LABELTIP | LVS_EX_GRIDLINES);
        SetWindowTheme(tree_, L"Explorer", nullptr);
        SetWindowTheme(list_, L"Explorer", nullptr);

        add_column(0, L"Name", 260);
        add_column(1, L"Type", 90);
        add_column(2, L"Size", 110);
        add_column(3, L"Sub", 70);
        add_column(4, L"Inode", 100);
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

        auto device = std::make_unique<ps2hdd::FileBlockDevice>(std::filesystem::path(path.data()));
        if (!device->is_open()) {
            message(L"Could not open the selected disk image.", MB_ICONERROR);
            return false;
        }
        return load_device(std::move(device));
    }

    bool open_physical(unsigned index)
    {
        auto device = std::make_unique<ps2hdd::PhysicalDrive>(index);
        if (!device->is_open()) {
            std::wostringstream text;
            text << L"Could not open \\\\.\\PhysicalDrive" << index
                 << L" for read-only access.\n\nRun DriveForge as Administrator if required.";
            message(text.str(), MB_ICONERROR);
            return false;
        }
        return load_device(std::move(device));
    }

    void tree_selection_changed(const NMTREEVIEWW& notification)
    {
        const auto data = static_cast<std::size_t>(notification.itemNew.lParam);
        if (data == 0) {
            show_drive_overview();
            return;
        }
        const std::size_t index = data - 1;
        if (index >= main_partitions_.size()) {
            return;
        }
        open_partition(main_partitions_[index]);
    }

    void list_double_click()
    {
        if (!pfs_) {
            return;
        }
        const int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0) {
            return;
        }

        if (!active_path_.empty()) {
            if (selected == 0) {
                navigate(parent_pfs_path(active_path_));
                return;
            }
        }
        const std::size_t offset = active_path_.empty() ? 0U : 1U;
        const std::size_t entry_index = static_cast<std::size_t>(selected) - offset;
        if (entry_index >= visible_entries_.size()) {
            return;
        }
        const auto& visible = visible_entries_[entry_index];
        if (visible.directory) {
            navigate(join_pfs_path(active_path_, visible.entry.name));
        } else if (visible.regular) {
            extract_entry(visible);
        }
    }

    void about()
    {
        std::wostringstream text;
        text << L"PS2 DriveForge " << widen(ps2hdd::version::string) << L"-dev ("
             << widen(ps2hdd::version::codename) << L")\n\n"
             << L"Native read-only APA/PFS browser for PlayStation 2 HDDs.\n\n"
             << L"Physical drives are opened with GENERIC_READ only.";
        message(text.str(), MB_ICONINFORMATION);
    }

private:
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
        SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(std::wstring(text).c_str()));
    }

    bool load_device(std::unique_ptr<ps2hdd::BlockDevice> candidate)
    {
        ps2hdd::apa::Reader reader(*candidate);
        auto scan = reader.scan();
        if (!scan.mbr_valid) {
            message(L"The selected source does not contain a valid PS2 APA MBR.", MB_ICONERROR);
            return false;
        }
        if (!scan.ok()) {
            const int answer = MessageBoxW(window_,
                                           L"APA diagnostics contain errors. DriveForge will keep the source read-only.\n\nOpen it anyway for inspection?",
                                           L"PS2 DriveForge", MB_YESNO | MB_ICONWARNING);
            if (answer != IDYES) {
                return false;
            }
        }

        pfs_.reset();
        volume_.reset();
        device_ = std::move(candidate);
        scan_ = std::move(scan);
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

        TVINSERTSTRUCTW root_insert{};
        root_insert.hParent = TVI_ROOT;
        root_insert.hInsertAfter = TVI_LAST;
        root_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        std::wstring root_text = widen(device_->display_name());
        root_insert.item.pszText = root_text.data();
        root_insert.item.lParam = 0;
        const HTREEITEM root = TreeView_InsertItem(tree_, &root_insert);

        for (const auto& partition : scan_.partitions) {
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
        pfs_.reset();
        volume_.reset();
        active_partition_.reset();
        active_path_.clear();
        visible_entries_.clear();
        ListView_DeleteAllItems(list_);

        insert_list_row(0, L"PS2 APA HDD", L"Drive", format_bytes(device_->size_bytes()), L"", L"");
        insert_list_row(1, L"APA version", L"Metadata", std::to_wstring(scan_.apa_version), L"", L"");
        insert_list_row(2, L"Main partitions", L"Metadata", std::to_wstring(main_partitions_.size()), L"", L"");

        std::wostringstream status;
        status << widen(device_->display_name()) << L"  |  APA v" << scan_.apa_version
               << L"  |  " << main_partitions_.size() << L" main partitions  |  READ ONLY";
        set_status(status.str());
    }

    void open_partition(const ps2hdd::apa::Partition& partition)
    {
        pfs_.reset();
        volume_.reset();
        active_partition_ = partition;
        active_path_.clear();
        visible_entries_.clear();
        ListView_DeleteAllItems(list_);

        if (partition.type != ps2hdd::apa::kTypePfs) {
            insert_list_row(0, widen(partition.id), widen(ps2hdd::apa::type_name(partition.type)),
                            format_bytes(partition.size_bytes()), L"", L"");
            set_status(widen(partition.id) + L"  |  Not a PFS filesystem  |  READ ONLY");
            return;
        }

        volume_ = std::make_unique<ps2hdd::ApaVolume>(*device_, partition);
        pfs_ = std::make_unique<ps2hdd::pfs::Reader>(*volume_);
        if (!pfs_->valid()) {
            insert_list_row(0, L"PFS mount failed", L"Error", L"", L"", L"");
            set_status(widen(partition.id) + L"  |  PFS error: " + widen(pfs_->last_error()));
            return;
        }
        navigate("");
    }

    void navigate(std::string path)
    {
        if (!pfs_ || !active_partition_) {
            return;
        }
        auto node = pfs_->resolve(path);
        if (!node) {
            message(L"Could not resolve PFS path:\n" + widen(pfs_->last_error()), MB_ICONERROR);
            return;
        }
        if ((node->inode.mode & ps2hdd::pfs::kModeMask) != ps2hdd::pfs::kModeDirectory) {
            return;
        }

        auto entries = pfs_->list_directory(*node);
        if (!pfs_->last_error().empty()) {
            message(L"Could not enumerate PFS directory:\n" + widen(pfs_->last_error()), MB_ICONERROR);
            return;
        }

        active_path_ = std::move(path);
        visible_entries_.clear();
        visible_entries_.reserve(entries.size());
        for (const auto& entry : entries) {
            VisibleEntry visible;
            visible.entry = entry;
            if (auto child = pfs_->read_inode(entry.inode)) {
                visible.inode_readable = true;
                visible.size = child->inode.size;
                const auto type = child->inode.mode & ps2hdd::pfs::kModeMask;
                visible.directory = type == ps2hdd::pfs::kModeDirectory;
                visible.regular = type == ps2hdd::pfs::kModeRegular;
            } else {
                visible.directory = entry.is_directory();
                visible.regular = entry.is_regular();
            }
            visible_entries_.push_back(std::move(visible));
        }

        std::sort(visible_entries_.begin(), visible_entries_.end(), [](const auto& a, const auto& b) {
            if (a.directory != b.directory) {
                return a.directory > b.directory;
            }
            return a.entry.name < b.entry.name;
        });

        ListView_DeleteAllItems(list_);
        int row = 0;
        if (!active_path_.empty()) {
            insert_list_row(row++, L"..", L"Folder", L"", L"", L"");
        }
        for (const auto& visible : visible_entries_) {
            const std::wstring type = visible.directory ? L"Folder" : (visible.regular ? L"File" : L"Other");
            insert_list_row(row++, widen(visible.entry.name), type,
                            visible.inode_readable ? format_bytes(visible.size) : L"?",
                            std::to_wstring(visible.entry.inode.subpart),
                            std::to_wstring(visible.entry.inode.number));
        }

        std::wstring location = widen(active_partition_->id) + L":/" + widen(active_path_);
        std::wostringstream status;
        status << location << L"  |  " << visible_entries_.size() << L" entries  |  READ ONLY"
               << L"  |  Double-click a folder to open, a file to extract";
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

    void extract_entry(const VisibleEntry& visible)
    {
        if (!pfs_ || !visible.regular) {
            return;
        }
        const std::string source_path = join_pfs_path(active_path_, visible.entry.name);
        auto node = pfs_->resolve(source_path);
        if (!node) {
            message(L"Could not resolve source file:\n" + widen(pfs_->last_error()), MB_ICONERROR);
            return;
        }

        std::array<wchar_t, 32768> output{};
        const auto default_name = widen(visible.entry.name);
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

        const std::filesystem::path output_path(output.data());
        std::ofstream stream(output_path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            message(L"Could not create the output file.", MB_ICONERROR);
            return;
        }

        constexpr std::size_t kChunk = 1024 * 1024;
        std::vector<std::byte> buffer(kChunk);
        std::uint64_t offset = 0;
        bool ok = true;
        while (offset < node->inode.size) {
            const auto take = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), node->inode.size - offset));
            auto chunk = std::span<std::byte>(buffer.data(), take);
            if (!pfs_->read(*node, offset, chunk)) {
                ok = false;
                break;
            }
            stream.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(take));
            if (!stream) {
                ok = false;
                break;
            }
            offset += take;
        }
        stream.close();

        if (!ok) {
            std::error_code ec;
            std::filesystem::remove(output_path, ec);
            message(L"Extraction failed. Partial output was removed.\n\n" + widen(pfs_->last_error()),
                    MB_ICONERROR);
            return;
        }
        std::wostringstream text;
        text << L"Extracted " << format_bytes(node->inode.size) << L" to:\n" << output_path.wstring();
        message(text.str(), MB_ICONINFORMATION);
    }

    HWND window_{};
    HWND tree_{};
    HWND list_{};
    HWND status_{};
    std::unique_ptr<ps2hdd::BlockDevice> device_;
    ps2hdd::apa::ScanResult scan_{};
    std::vector<ps2hdd::apa::Partition> main_partitions_;
    std::optional<ps2hdd::apa::Partition> active_partition_;
    std::unique_ptr<ps2hdd::ApaVolume> volume_;
    std::unique_ptr<ps2hdd::pfs::Reader> pfs_;
    std::string active_path_;
    std::vector<VisibleEntry> visible_entries_;
};

HMENU create_menu()
{
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU physical = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kIdOpenImage, L"Open disk &image...\tCtrl+O");
    for (UINT i = 0; i < kPhysicalCount; ++i) {
        std::wstring label = L"PhysicalDrive" + std::to_wstring(i);
        AppendMenuW(physical, MF_STRING, kIdPhysicalBase + i, label.c_str());
    }
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(physical), L"Open &physical drive");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kIdExit, L"E&xit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

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
    case WM_SIZE:
        if (app) {
            app->resize(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_COMMAND:
        if (!app) {
            break;
        }
        if (LOWORD(wparam) == kIdOpenImage) {
            app->open_image_dialog();
            return 0;
        }
        if (LOWORD(wparam) == kIdExit) {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wparam) == kIdAbout) {
            app->about();
            return 0;
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

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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

    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (app) {
        app->open_image_dialog();
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
