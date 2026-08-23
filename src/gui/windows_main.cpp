#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ps2hdd/drive_session.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/partition_catalog.hpp"
#include "ps2hdd/physical_discovery.hpp"
#include "ps2hdd/physical_drive.hpp"
#include "ps2hdd/version.hpp"
#ifdef PS2DF_HAS_DOKANY
#include "ps2hdd/dokany_mount.hpp"
#endif
#include "windows_elevation.hpp"
#include "windows_theme.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
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
#include <thread>
#include <utility>
#include <vector>

namespace {

using ps2driveforge::gui::ThemePreference;

constexpr wchar_t kWindowClass[] = L"PS2DriveForgeMainWindow";
constexpr UINT kIdOpenImage = 100;
constexpr UINT kIdExit = 101;
constexpr UINT kIdAbout = 102;
constexpr UINT kIdRescanPhysical = 103;
constexpr UINT kIdThemeSystem = 104;
constexpr UINT kIdThemeLight = 105;
constexpr UINT kIdThemeDark = 106;
constexpr UINT kIdMountReadOnly = 107;
constexpr UINT kIdOpenMounted = 108;
constexpr UINT kIdUnmount = 109;
constexpr UINT kIdRestartElevated = 110;
constexpr UINT kIdPhysicalBase = 300;
constexpr UINT kMaxPhysicalMenuEntries = 512;
constexpr UINT kMountTimerId = 1;
constexpr UINT kDiscoveryComplete = WM_APP + 1;
constexpr int kTreeId = 1000;
constexpr int kListId = 1001;
constexpr int kStatusId = 1002;

HMENU g_physical_menu = nullptr;
std::vector<unsigned> g_physical_menu_indices;

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

struct TreeTarget {
    std::uint32_t start_lba{};
    bool is_sub{};
};

class App {
public:
    explicit App(HWND window, bool elevation_limited)
        : window_(window), elevation_limited_(elevation_limited),
          theme_preference_(ps2driveforge::gui::load_theme_preference())
    {
    }

    ~App()
    {
        if (discovery_thread_.joinable()) {
            discovery_thread_.join();
        }
#ifdef PS2DF_HAS_DOKANY
        if (mount_controller_) {
            if (mount_controller_->is_running()) {
                (void)mount_controller_->request_unmount();
            }
            mount_controller_->wait();
        }
#endif
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
        status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"Starting PS2 HDD discovery — READ ONLY",
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
        update_action_menu();
        start_discovery();
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

    void system_settings_changed() { apply_theme(false); }

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

        const std::filesystem::path image_path(path.data());
        auto source = std::make_unique<ps2hdd::FileBlockDevice>(image_path);
        if (!source->is_open()) {
            message(L"Could not open the selected disk image.", MB_ICONERROR);
            return false;
        }
        if (!load_device(std::move(source))) {
            return false;
        }
        current_image_ = image_path;
        current_physical_.reset();
        update_action_menu();
        return true;
    }

    bool open_physical(unsigned index)
    {
        auto source = std::make_unique<ps2hdd::PhysicalDrive>(index);
        if (!source->is_open()) {
            std::wostringstream text;
            text << L"Could not open \\\\.\\PhysicalDrive" << index
                 << L" for read-only access (Win32 error " << source->open_error() << L").";
            if (!ps2driveforge::gui::is_process_elevated()) {
                text << L"\n\nRestart DriveForge as Administrator to access raw disks.";
            }
            message(text.str(), MB_ICONERROR);
            return false;
        }
        if (!load_device(std::move(source))) {
            return false;
        }
        current_physical_ = index;
        current_image_.reset();
        update_action_menu();
        return true;
    }

    void restart_elevated()
    {
        const auto attempt = ps2driveforge::gui::relaunch_elevated(false);
        if (attempt == ps2driveforge::gui::ElevationAttempt::relaunched) {
            DestroyWindow(window_);
        } else if (attempt == ps2driveforge::gui::ElevationAttempt::cancelled) {
            message(L"Administrator elevation was cancelled. Disk images remain available, but raw PhysicalDrive access may be unavailable.", MB_ICONINFORMATION);
        } else if (attempt == ps2driveforge::gui::ElevationAttempt::failed &&
                   !ps2driveforge::gui::is_process_elevated()) {
            message(L"Could not restart DriveForge with Administrator privileges.", MB_ICONERROR);
        }
    }

    void start_discovery()
    {
        if (discovery_running_) {
            return;
        }
        if (discovery_thread_.joinable()) {
            discovery_thread_.join();
        }
        discovery_running_ = true;
        detected_drives_.clear();
        rebuild_physical_menu();
        set_status(L"Scanning Windows disk interfaces for PS2 APA volumes — READ ONLY");

        const HWND target = window_;
        discovery_thread_ = std::thread([target] {
            auto results = std::make_unique<std::vector<ps2hdd::PhysicalDriveProbe>>(
                ps2hdd::discover_physical_drives());
            auto* raw = results.release();
            if (!PostMessageW(target, kDiscoveryComplete, 0, reinterpret_cast<LPARAM>(raw))) {
                delete raw;
            }
        });
    }

    void discovery_complete(std::unique_ptr<std::vector<ps2hdd::PhysicalDriveProbe>> probes)
    {
        if (discovery_thread_.joinable()) {
            discovery_thread_.join();
        }
        discovery_running_ = false;
        all_probes_ = std::move(*probes);
        detected_drives_.clear();
        bool access_denied = false;
        for (const auto& probe : all_probes_) {
            access_denied = access_denied || probe.access_denied;
            if (probe.apa_detected) {
                detected_drives_.push_back(probe);
            }
        }
        rebuild_physical_menu();

        if (detected_drives_.empty()) {
            if (access_denied && !ps2driveforge::gui::is_process_elevated()) {
                set_status(L"No readable PS2 APA disk found — raw disk access denied; restart as Administrator");
            } else {
                set_status(L"No PS2 APA HDD detected — use File > PS2 HDDs > Rescan after connecting a disk");
            }
            return;
        }

        if (!session_ && detected_drives_.size() == 1) {
            open_physical(detected_drives_.front().index);
            return;
        }

        std::wostringstream status;
        status << L"Detected " << detected_drives_.size() << L" PS2 APA HDD"
               << (detected_drives_.size() == 1 ? L"" : L"s")
               << L" — select one from File > PS2 HDDs";
        set_status(status.str());
    }

    void tree_selection_changed(const NMTREEVIEWW& notification)
    {
        const auto data = static_cast<std::size_t>(notification.itemNew.lParam);
        if (data == 0) {
            show_drive_overview();
            return;
        }
        const std::size_t index = data - 1;
        if (index >= tree_targets_.size()) {
            return;
        }
        const auto& target = tree_targets_[index];
        const auto* partition = partition_at_lba(target.start_lba);
        if (partition == nullptr) {
            show_drive_overview();
            return;
        }
        if (target.is_sub) {
            open_subpartition(*partition);
        } else {
            open_partition(*partition);
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

#ifdef PS2DF_HAS_DOKANY
    void mount_read_only()
    {
        if (!session_) {
            message(L"Open a PS2 HDD or disk image before mounting.", MB_ICONINFORMATION);
            return;
        }
        if (mount_controller_ && mount_controller_->is_running()) {
            message(L"A DriveForge filesystem is already mounted.", MB_ICONINFORMATION);
            return;
        }

        std::unique_ptr<ps2hdd::BlockDevice> source;
        if (current_physical_) {
            auto physical = std::make_unique<ps2hdd::PhysicalDrive>(*current_physical_);
            if (!physical->is_open()) {
                message(L"Could not reopen the selected PS2 HDD read-only for mounting.", MB_ICONERROR);
                return;
            }
            source = std::move(physical);
        } else if (current_image_) {
            auto image = std::make_unique<ps2hdd::FileBlockDevice>(*current_image_);
            if (!image->is_open()) {
                message(L"Could not reopen the selected disk image for mounting.", MB_ICONERROR);
                return;
            }
            source = std::move(image);
        } else {
            message(L"The current source cannot be reopened for mounting.", MB_ICONERROR);
            return;
        }

        const auto point = ps2hdd::DokanyMountController::suggest_mount_point(L'P');
        if (point.empty()) {
            message(L"No free drive letter from D: through Z: is available.", MB_ICONERROR);
            return;
        }

        mount_controller_ = std::make_unique<ps2hdd::DokanyMountController>();
        if (!mount_controller_->start(std::move(source), point, false)) {
            message(L"Could not start the read-only mount:\n\n" + widen(mount_controller_->last_error()), MB_ICONERROR);
            mount_controller_.reset();
            return;
        }
        SetTimer(window_, kMountTimerId, 100, nullptr);
        set_status(L"Mounting PS2 DriveForge read-only at " + point + L"...");
        update_action_menu();
    }

    void open_mounted_volume()
    {
        if (!mount_controller_ || !mount_controller_->is_mounted()) {
            return;
        }
        const auto point = mount_controller_->mount_point();
        ShellExecuteW(window_, L"open", point.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void unmount()
    {
        if (!mount_controller_) {
            return;
        }
        if (mount_controller_->is_running()) {
            if (!mount_controller_->request_unmount()) {
                message(L"Dokany did not accept the unmount request.", MB_ICONERROR);
                return;
            }
            SetTimer(window_, kMountTimerId, 100, nullptr);
            set_status(L"Unmounting PS2 DriveForge filesystem...");
        } else {
            finish_mount_controller();
        }
        update_action_menu();
    }

    void poll_mount()
    {
        if (!mount_controller_) {
            KillTimer(window_, kMountTimerId);
            return;
        }
        if (mount_controller_->is_mounted()) {
            KillTimer(window_, kMountTimerId);
            set_status(L"Mounted read-only at " + mount_controller_->mount_point() +
                       L" — use File > Open mounted volume to browse in Explorer");
            update_action_menu();
            return;
        }
        if (!mount_controller_->is_running()) {
            KillTimer(window_, kMountTimerId);
            const auto error = mount_controller_->last_error();
            if (!error.empty()) {
                message(L"Dokany mount stopped:\n\n" + widen(error), MB_ICONERROR);
            }
            finish_mount_controller();
        }
    }
#endif

    void about()
    {
        std::wostringstream text;
        text << L"PS2 DriveForge " << widen(ps2hdd::version::string) << L"-dev ("
             << widen(ps2hdd::version::codename) << L")\n\n"
             << L"Native read-only APA/PFS browser and Explorer mount for PlayStation 2 HDDs.\n\n"
             << L"Windows disk devices are enumerated through SetupAPI and classified by the DriveForge APA parser.\n"
             << L"APA sub-partitions are grouped beneath their authoritative main partition by main_lba.\n"
             << L"Physical drives are opened with GENERIC_READ only.\n"
             << L"Theme: System / Light / Dark with High Contrast passthrough.";
        message(text.str(), MB_ICONINFORMATION);
    }

private:
    void update_theme_menu() const
    {
        HMENU menu = GetMenu(window_);
        HMENU view = menu ? GetSubMenu(menu, 1) : nullptr;
        HMENU theme = view ? GetSubMenu(view, 0) : nullptr;
        if (theme) {
            CheckMenuRadioItem(theme, kIdThemeSystem, kIdThemeDark,
                               theme_command(theme_preference_), MF_BYCOMMAND);
        }
    }

    void update_action_menu() const
    {
        HMENU menu = GetMenu(window_);
        if (!menu) {
            return;
        }
        const bool source_open = session_ != nullptr;
#ifdef PS2DF_HAS_DOKANY
        const bool controller = mount_controller_ != nullptr;
        const bool mounted = controller && mount_controller_->is_mounted();
        const bool running = controller && mount_controller_->is_running();
        EnableMenuItem(menu, kIdMountReadOnly, MF_BYCOMMAND | (source_open && !running ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menu, kIdOpenMounted, MF_BYCOMMAND | (mounted ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menu, kIdUnmount, MF_BYCOMMAND | (running ? MF_ENABLED : MF_GRAYED));
#else
        EnableMenuItem(menu, kIdMountReadOnly, MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(menu, kIdOpenMounted, MF_BYCOMMAND | MF_GRAYED);
        EnableMenuItem(menu, kIdUnmount, MF_BYCOMMAND | MF_GRAYED);
#endif
        EnableMenuItem(menu, kIdRestartElevated, MF_BYCOMMAND |
                       (!ps2driveforge::gui::is_process_elevated() ? MF_ENABLED : MF_GRAYED));
        DrawMenuBar(window_);
    }

    void rebuild_physical_menu()
    {
        if (!g_physical_menu) {
            return;
        }
        while (GetMenuItemCount(g_physical_menu) > 0) {
            DeleteMenu(g_physical_menu, 0, MF_BYPOSITION);
        }
        g_physical_menu_indices.clear();

        if (discovery_running_) {
            AppendMenuW(g_physical_menu, MF_STRING | MF_GRAYED, 0, L"Scanning for PS2 HDDs...");
        } else if (detected_drives_.empty()) {
            AppendMenuW(g_physical_menu, MF_STRING | MF_GRAYED, 0, L"No PS2 APA HDD detected");
        } else {
            const std::size_t count = std::min<std::size_t>(detected_drives_.size(), kMaxPhysicalMenuEntries);
            g_physical_menu_indices.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& probe = detected_drives_[i];
                g_physical_menu_indices.push_back(probe.index);
                std::wstring label = probe.friendly_name.empty()
                    ? L"PhysicalDrive" + std::to_wstring(probe.index)
                    : widen(probe.friendly_name);
                label += L" — " + format_bytes(probe.size_bytes);
                label += L" — APA v" + std::to_wstring(probe.apa_version);
                label += L" — " + std::to_wstring(probe.main_partition_count) + L" partitions";
                AppendMenuW(g_physical_menu, MF_STRING,
                            kIdPhysicalBase + static_cast<UINT>(i), label.c_str());
            }
        }
        AppendMenuW(g_physical_menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(g_physical_menu, MF_STRING, kIdRescanPhysical, L"&Rescan PS2 HDDs");
        DrawMenuBar(window_);
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

    const ps2hdd::apa::Partition* partition_at_lba(std::uint32_t lba) const noexcept
    {
        if (!session_) {
            return nullptr;
        }
        for (const auto& partition : session_->scan_result().partitions) {
            if (partition.start_lba == lba) {
                return &partition;
            }
        }
        return nullptr;
    }

    const ps2hdd::PartitionCatalogGroup* group_at_main_lba(std::uint32_t lba) const noexcept
    {
        for (const auto& group : partition_grouping_.groups) {
            if (group.main.start_lba == lba) {
                return &group;
            }
        }
        return nullptr;
    }

    LPARAM add_tree_target(std::uint32_t start_lba, bool is_sub)
    {
        tree_targets_.push_back({start_lba, is_sub});
        return static_cast<LPARAM>(tree_targets_.size());
    }

    bool load_device(std::unique_ptr<ps2hdd::BlockDevice> source)
    {
#ifdef PS2DF_HAS_DOKANY
        if (mount_controller_ && mount_controller_->is_running()) {
            message(L"Unmount the current Explorer volume before switching source devices.", MB_ICONWARNING);
            return false;
        }
#endif
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
        partition_grouping_ = {};
        tree_targets_.clear();
        if (!session_) {
            return;
        }

        partition_grouping_ = ps2hdd::group_partition_catalog(session_->partition_catalog(true));

        TVINSERTSTRUCTW root_insert{};
        root_insert.hParent = TVI_ROOT;
        root_insert.hInsertAfter = TVI_LAST;
        root_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        std::wstring root_text = widen(session_->device().display_name());
        root_insert.item.pszText = root_text.data();
        root_insert.item.lParam = 0;
        const HTREEITEM root = TreeView_InsertItem(tree_, &root_insert);

        for (const auto& group : partition_grouping_.groups) {
            std::wstring text = widen(group.main.id.empty() ? "<unnamed>" : group.main.id);
            if (group.is_hdl_game()) {
                text += L"  [HDL game";
            } else {
                text += L"  [" + widen(ps2hdd::apa::type_name(group.main.raw_type));
            }
            if (!group.sub_partitions.empty()) {
                text += L", " + std::to_wstring(group.sub_partitions.size() + 1) + L" parts";
            }
            text += L"]  " + format_bytes(group.logical_size_bytes);
            if (!group.complete) {
                text += L"  [incomplete]";
            }

            TVINSERTSTRUCTW insert{};
            insert.hParent = root;
            insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT | TVIF_PARAM;
            insert.item.pszText = text.data();
            insert.item.lParam = add_tree_target(group.main.start_lba, false);
            const HTREEITEM parent = TreeView_InsertItem(tree_, &insert);

            for (const auto& child : group.sub_partitions) {
                std::wstring child_text = L"Sub-partition #" + std::to_wstring(child.number);
                if (!child.id.empty()) {
                    child_text += L" — " + widen(child.id);
                }
                child_text += L"  [" + widen(ps2hdd::apa::type_name(child.raw_type)) + L"]  ";
                child_text += format_bytes(child.extent_size_bytes);

                TVINSERTSTRUCTW child_insert{};
                child_insert.hParent = parent;
                child_insert.hInsertAfter = TVI_LAST;
                child_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
                child_insert.item.pszText = child_text.data();
                child_insert.item.lParam = add_tree_target(child.start_lba, true);
                TreeView_InsertItem(tree_, &child_insert);
            }
        }

        if (!partition_grouping_.orphan_sub_partitions.empty()) {
            std::wstring orphan_text = L"Orphan sub-partitions (" +
                                       std::to_wstring(partition_grouping_.orphan_sub_partitions.size()) +
                                       L") — diagnostic";
            TVINSERTSTRUCTW orphan_insert{};
            orphan_insert.hParent = root;
            orphan_insert.hInsertAfter = TVI_LAST;
            orphan_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
            orphan_insert.item.pszText = orphan_text.data();
            orphan_insert.item.lParam = 0;
            const HTREEITEM orphan_root = TreeView_InsertItem(tree_, &orphan_insert);

            for (const auto& child : partition_grouping_.orphan_sub_partitions) {
                std::wstring child_text = L"LBA " + std::to_wstring(child.start_lba) +
                                          L" — main LBA " + std::to_wstring(child.main_lba) +
                                          L" — " + format_bytes(child.extent_size_bytes);
                TVINSERTSTRUCTW child_insert{};
                child_insert.hParent = orphan_root;
                child_insert.hInsertAfter = TVI_LAST;
                child_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
                child_insert.item.pszText = child_text.data();
                child_insert.item.lParam = add_tree_target(child.start_lba, true);
                TreeView_InsertItem(tree_, &child_insert);
            }
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
            set_status(elevation_limited_ ? L"No PS2 HDD opened — limited non-admin mode — READ ONLY"
                                         : L"No PS2 HDD opened — READ ONLY");
            return;
        }
        const auto& scan = session_->scan_result();
        std::size_t grouped_subs = 0;
        for (const auto& group : partition_grouping_.groups) {
            grouped_subs += group.sub_partitions.size();
        }
        insert_list_row(0, L"PS2 APA HDD", L"Drive", format_bytes(session_->device().size_bytes()), L"", L"");
        insert_list_row(1, L"APA version", L"Metadata", std::to_wstring(scan.apa_version), L"", L"");
        insert_list_row(2, L"Logical/main partitions", L"Metadata",
                        std::to_wstring(partition_grouping_.groups.size()), L"", L"");
        insert_list_row(3, L"Grouped sub-partitions", L"Metadata",
                        std::to_wstring(grouped_subs), L"", L"");
        insert_list_row(4, L"Orphan sub-partitions", L"Diagnostic",
                        std::to_wstring(partition_grouping_.orphan_sub_partitions.size()), L"", L"");
        insert_list_row(5, L"Diagnostics", L"Metadata", scan.ok() ? L"clean" : L"errors", L"", L"");
        std::wostringstream status;
        status << widen(session_->device().display_name()) << L"  |  APA v" << scan.apa_version
               << L"  |  " << partition_grouping_.groups.size() << L" logical partitions"
               << L"  |  " << grouped_subs << L" grouped sub-partitions";
        if (!partition_grouping_.orphan_sub_partitions.empty()) {
            status << L"  |  " << partition_grouping_.orphan_sub_partitions.size() << L" orphan";
        }
        status << L"  |  READ ONLY" << io_suffix();
        set_status(status.str());
    }

    void open_partition(const ps2hdd::apa::Partition& partition)
    {
        active_partition_ = partition;
        active_path_.clear();
        visible_entries_.clear();
        ListView_DeleteAllItems(list_);
        if (partition.type != ps2hdd::apa::kTypePfs) {
            if (const auto* group = group_at_main_lba(partition.start_lba);
                group != nullptr && !group->sub_partitions.empty()) {
                const std::wstring display_name = widen(partition.id.empty() ? "<unnamed>" : partition.id);
                insert_list_row(0, display_name,
                                group->is_hdl_game() ? L"HDL game" : widen(ps2hdd::apa::type_name(partition.type)),
                                format_bytes(group->logical_size_bytes),
                                std::to_wstring(group->sub_partitions.size()), L"");
                insert_list_row(1, L"Main APA extent", L"Allocation",
                                format_bytes(group->main.extent_size_bytes), L"0", L"");
                insert_list_row(2, L"Sub-partition extents", L"Allocation",
                                format_bytes(group->subpartition_bytes),
                                std::to_wstring(group->sub_partitions.size()), L"");
                insert_list_row(3, L"Ownership map", L"Metadata",
                                group->complete ? L"complete" : L"incomplete", L"", L"");
                std::wostringstream status;
                status << display_name << L"  |  " << format_bytes(group->logical_size_bytes)
                       << L" total  |  " << group->sub_partitions.size() << L" sub-partitions"
                       << (group->complete ? L"" : L"  |  INCOMPLETE OWNERSHIP MAP")
                       << L"  |  READ ONLY" << io_suffix();
                set_status(status.str());
                return;
            }

            insert_list_row(0, widen(partition.id), widen(ps2hdd::apa::type_name(partition.type)),
                            format_bytes(partition.size_bytes()), L"", L"");
            set_status(widen(partition.id) + L"  |  Not a PFS filesystem  |  READ ONLY" + io_suffix());
            return;
        }
        navigate("");
    }

    void open_subpartition(const ps2hdd::apa::Partition& partition)
    {
        active_partition_.reset();
        active_path_.clear();
        visible_entries_.clear();
        ListView_DeleteAllItems(list_);

        const auto* owner = partition_at_lba(partition.main_lba);
        const std::wstring owner_name = owner != nullptr
                                            ? widen(owner->id.empty() ? "<unnamed main>" : owner->id)
                                            : L"<unresolved main>";
        const std::wstring child_name = partition.id.empty()
                                            ? L"Sub-partition #" + std::to_wstring(partition.number)
                                            : widen(partition.id);
        const auto extent_bytes =
            static_cast<std::uint64_t>(partition.length_sectors) * ps2hdd::apa::kSectorSize;

        insert_list_row(0, child_name, widen(ps2hdd::apa::type_name(partition.type)),
                        format_bytes(extent_bytes), std::to_wstring(partition.number), L"");
        insert_list_row(1, L"Owned by", L"APA main", owner_name, L"", L"");
        insert_list_row(2, L"Start LBA", L"Metadata", std::to_wstring(partition.start_lba), L"", L"");
        insert_list_row(3, L"Main LBA", L"Metadata", std::to_wstring(partition.main_lba), L"", L"");

        std::wostringstream status;
        status << L"Sub-partition #" << partition.number << L" of " << owner_name
               << L"  |  " << format_bytes(extent_bytes)
               << (owner != nullptr ? L"  |  grouped by authoritative main_lba" : L"  |  ORPHAN")
               << L"  |  READ ONLY" << io_suffix();
        set_status(status.str());
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

#ifdef PS2DF_HAS_DOKANY
    void finish_mount_controller()
    {
        if (!mount_controller_) {
            return;
        }
        mount_controller_->wait();
        mount_controller_.reset();
        show_drive_overview();
        update_action_menu();
    }
#endif

    HWND window_{};
    HWND tree_{};
    HWND list_{};
    HWND status_{};
    HBRUSH background_brush_{};
    bool elevation_limited_{};
    bool discovery_running_{};
    ThemePreference theme_preference_{ThemePreference::System};
    std::thread discovery_thread_;
    std::vector<ps2hdd::PhysicalDriveProbe> all_probes_;
    std::vector<ps2hdd::PhysicalDriveProbe> detected_drives_;
    std::unique_ptr<ps2hdd::DriveSession> session_;
    std::optional<unsigned> current_physical_;
    std::optional<std::filesystem::path> current_image_;
#ifdef PS2DF_HAS_DOKANY
    std::unique_ptr<ps2hdd::DokanyMountController> mount_controller_;
#endif
    ps2hdd::PartitionCatalogGrouping partition_grouping_;
    std::vector<TreeTarget> tree_targets_;
    std::optional<ps2hdd::apa::Partition> active_partition_;
    std::string active_path_;
    std::vector<ps2hdd::SessionEntry> visible_entries_;
};

HMENU create_menu()
{
    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    g_physical_menu = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, kIdOpenImage, L"Open disk &image...");
    AppendMenuW(g_physical_menu, MF_STRING | MF_GRAYED, 0, L"Scanning for PS2 HDDs...");
    AppendMenuW(g_physical_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_physical_menu, MF_STRING, kIdRescanPhysical, L"&Rescan PS2 HDDs");
    AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(g_physical_menu), L"PS2 &HDDs");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING | MF_GRAYED, kIdMountReadOnly, L"&Mount read-only...");
    AppendMenuW(file, MF_STRING | MF_GRAYED, kIdOpenMounted, L"Open mounted volume in &Explorer");
    AppendMenuW(file, MF_STRING | MF_GRAYED, kIdUnmount, L"&Unmount");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kIdRestartElevated, L"Restart as &Administrator");
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
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        const bool limited = reinterpret_cast<INT_PTR>(create->lpCreateParams) != 0;
        auto owned = std::make_unique<App>(window, limited);
        owned->create_controls();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owned.release()));
        return 0;
    }
    case kDiscoveryComplete:
        if (app) {
            std::unique_ptr<std::vector<ps2hdd::PhysicalDriveProbe>> results(
                reinterpret_cast<std::vector<ps2hdd::PhysicalDriveProbe>*>(lparam));
            app->discovery_complete(std::move(results));
        }
        return 0;
    case WM_TIMER:
#ifdef PS2DF_HAS_DOKANY
        if (app && wparam == kMountTimerId) {
            app->poll_mount();
            return 0;
        }
#endif
        break;
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
        case kIdRescanPhysical:
            app->start_discovery();
            return 0;
        case kIdRestartElevated:
            app->restart_elevated();
            return 0;
#ifdef PS2DF_HAS_DOKANY
        case kIdMountReadOnly:
            app->mount_read_only();
            return 0;
        case kIdOpenMounted:
            app->open_mounted_volume();
            return 0;
        case kIdUnmount:
            app->unmount();
            return 0;
#endif
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
        if (LOWORD(wparam) >= kIdPhysicalBase &&
            LOWORD(wparam) < kIdPhysicalBase + kMaxPhysicalMenuEntries) {
            const std::size_t menu_index = LOWORD(wparam) - kIdPhysicalBase;
            if (menu_index < g_physical_menu_indices.size()) {
                app->open_physical(g_physical_menu_indices[menu_index]);
            }
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
        KillTimer(window, kMountTimerId);
        delete app;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show)
{
    const bool relaunch_marker = command_line &&
        std::wstring_view(command_line).find(L"--elevated-relaunch") != std::wstring_view::npos;
    const auto elevation = ps2driveforge::gui::relaunch_elevated(relaunch_marker);
    if (elevation == ps2driveforge::gui::ElevationAttempt::relaunched) {
        return 0;
    }
    const bool elevation_limited = !ps2driveforge::gui::is_process_elevated();

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
                                  nullptr, create_menu(), instance,
                                  reinterpret_cast<void*>(static_cast<INT_PTR>(elevation_limited ? 1 : 0)));
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
