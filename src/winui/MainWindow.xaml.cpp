#include "pch.h"
#include "MainWindow.xaml.h"
#include "StartupLog.hpp"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "ps2hdd/darkness_policy.hpp"
#include "ps2hdd/pfs_export.hpp"

#include <microsoft.ui.xaml.window.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <exception>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace winrt::PS2DriveForge::WinUI::implementation
{
namespace
{
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

std::wstring widen(std::string_view value)
{
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          value.data(), static_cast<int>(value.size()),
                                          nullptr, 0);
    if (count <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        value.data(), static_cast<int>(value.size()),
                        result.data(), count);
    return result;
}

winrt::hstring htext(std::string_view value)
{
    return winrt::hstring(widen(value));
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
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
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << L' ' << units[unit];
    return out.str();
}

std::wstring format_ms(std::uint64_t ns)
{
    if (ns == 0) {
        return L"—";
    }
    std::wostringstream out;
    out << std::fixed << std::setprecision(2) << (static_cast<double>(ns) / 1'000'000.0) << L" ms";
    return out.str();
}

std::wstring format_ms(double ms)
{
    std::wostringstream out;
    out << std::fixed << std::setprecision(3) << ms << L" ms";
    return out.str();
}

std::wstring kind_name(ps2hdd::PartitionCatalogKind kind)
{
    switch (kind) {
    case ps2hdd::PartitionCatalogKind::mbr: return L"MBR";
    case ps2hdd::PartitionCatalogKind::pfs: return L"PFS";
    case ps2hdd::PartitionCatalogKind::hdl: return L"HDL";
    case ps2hdd::PartitionCatalogKind::free_space: return L"Free";
    case ps2hdd::PartitionCatalogKind::other:
    default: return L"Other";
    }
}

std::wstring enrichment_name(ps2hdd::EnrichmentState state)
{
    switch (state) {
    case ps2hdd::EnrichmentState::pending: return L"Pending";
    case ps2hdd::EnrichmentState::ready: return L"Ready";
    case ps2hdd::EnrichmentState::error: return L"Error";
    case ps2hdd::EnrichmentState::not_applicable:
    default: return L"—";
    }
}

std::wstring bus_name(const ps2hdd::StorageCharacteristics& storage)
{
    if (!storage.bus_type_known) {
        return L"Unknown";
    }
    switch (storage.bus_type) {
    case 1: return L"SCSI";
    case 2: return L"ATAPI";
    case 3: return L"ATA";
    case 7: return L"USB";
    case 11: return L"SATA";
    case 17: return L"NVMe";
    default: return L"Bus " + std::to_wstring(storage.bus_type);
    }
}

std::wstring media_name(const ps2hdd::StorageCharacteristics& storage)
{
    switch (storage.media_class) {
    case ps2hdd::StorageMediaClass::rotational: return L"Rotational";
    case ps2hdd::StorageMediaClass::solid_state: return L"Solid state";
    case ps2hdd::StorageMediaClass::unknown:
    default: return L"Unknown";
    }
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

TextBlock make_cell(std::wstring text, bool strong = false)
{
    TextBlock cell;
    cell.Text(text);
    cell.FontSize(12);
    cell.VerticalAlignment(VerticalAlignment::Center);
    cell.TextTrimming(TextTrimming::CharacterEllipsis);
    if (strong) {
        cell.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    }
    return cell;
}

Grid make_partition_row(const ps2df::winui::PartitionRowSnapshot& row)
{
    Grid grid;
    grid.Padding(Thickness{14, 8, 14, 8});
    constexpr std::array<double, 6> stars{2.4, 1.05, 1.0, 0.9, 0.85, 0.8};
    for (double weight : stars) {
        ColumnDefinition column;
        column.Width(GridLengthHelper::FromValueAndType(weight, GridUnitType::Star));
        grid.ColumnDefinitions().Append(column);
    }

    std::array<TextBlock, 6> cells{
        make_cell(widen(row.title.empty() ? row.partition_id : row.title), true),
        make_cell(widen(row.startup)),
        make_cell(kind_name(row.kind)),
        make_cell(format_bytes(row.size_bytes)),
        make_cell(std::to_wstring(row.start_lba)),
        make_cell(enrichment_name(row.enrichment)),
    };
    for (std::uint32_t i = 0; i < cells.size(); ++i) {
        Grid::SetColumn(cells[i], static_cast<int>(i));
        grid.Children().Append(cells[i]);
    }
    return grid;
}

Grid make_file_row(const ps2df::winui::BrowseEntrySnapshot& entry)
{
    Grid grid;
    grid.Padding(Thickness{14, 9, 14, 9});
    ColumnDefinition name; name.Width(GridLengthHelper::FromValueAndType(3, GridUnitType::Star));
    ColumnDefinition type; type.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    ColumnDefinition size; size.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    grid.ColumnDefinitions().Append(name);
    grid.ColumnDefinitions().Append(type);
    grid.ColumnDefinitions().Append(size);

    auto name_cell = make_cell(widen(entry.name), true);
    auto type_cell = make_cell(entry.is_directory ? L"Folder" : (entry.is_regular ? L"File" : L"Other"));
    auto size_cell = make_cell(entry.is_directory ? L"—" : format_bytes(entry.size_bytes));
    Grid::SetColumn(type_cell, 1);
    Grid::SetColumn(size_cell, 2);
    grid.Children().Append(name_cell);
    grid.Children().Append(type_cell);
    grid.Children().Append(size_cell);
    return grid;
}

std::filesystem::path local_app_data_path()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size()) {
        return std::filesystem::temp_directory_path();
    }
    return std::filesystem::path(buffer.data());
}

bool path_exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

} // namespace

MainWindow::MainWindow()
{
    ps2df::winui::diag::log("MainWindow::MainWindow entered");
    try {
        ps2df::winui::diag::log("MainWindow::InitializeComponent begin");
        InitializeComponent();
        ps2df::winui::diag::log("MainWindow::InitializeComponent complete");

        ExtendsContentIntoTitleBar(true);
        SetTitleBar(TitleBarDragRegion());
        wire_events();
        load_theme_preference();

        telemetry_timer_ = DispatcherTimer();
        telemetry_timer_.Interval(std::chrono::seconds(1));
        telemetry_timer_.Tick({this, &MainWindow::on_telemetry_tick});
        telemetry_timer_.Start();

        mount_timer_ = DispatcherTimer();
        mount_timer_.Interval(std::chrono::milliseconds(250));
        mount_timer_.Tick({this, &MainWindow::on_mount_tick});

        start_discovery(false);
    } catch (winrt::hresult_error const& error) {
        const auto message = error.message();
        ps2df::winui::diag::log_hresult("MainWindow construction failed", error.code().value,
                                       std::wstring_view(message.c_str(), message.size()));
        throw;
    } catch (std::exception const& error) {
        ps2df::winui::diag::log(std::string("MainWindow construction std::exception: ") + error.what());
        throw;
    } catch (...) {
        ps2df::winui::diag::log("MainWindow construction failed with unknown exception");
        throw;
    }
    ps2df::winui::diag::log("MainWindow::MainWindow complete");
}

MainWindow::~MainWindow()
{
    if (discovery_thread_.joinable()) discovery_thread_.request_stop();
    if (source_thread_.joinable()) source_thread_.request_stop();
    if (enrichment_thread_.joinable()) enrichment_thread_.request_stop();
    if (export_thread_.joinable()) export_thread_.request_stop();
    if (mount_process_) {
        CloseHandle(mount_process_);
        mount_process_ = nullptr;
    }
}

void MainWindow::wire_events()
{
    Navigation().ItemInvoked({this, &MainWindow::on_navigation_invoked});
    OpenImageButton().Click({this, &MainWindow::on_open_image});
    OpenImageInlineButton().Click({this, &MainWindow::on_open_image});
    RescanButton().Click({this, &MainWindow::on_rescan});
    ScanDrivesButton().Click({this, &MainWindow::on_rescan});
    OpenDriveButton().Click({this, &MainWindow::on_open_drive});
    FilterBox().TextChanged({this, &MainWindow::on_filter_changed});
    PartitionKindFilter().SelectionChanged({this, &MainWindow::on_partition_filter_changed});
    ShowSubsToggle().Toggled({this, &MainWindow::on_show_subs_toggled});
    RefreshMetadataButton().Click({this, &MainWindow::on_refresh_metadata});
    CancelEnrichmentButton().Click({this, &MainWindow::on_cancel_enrichment});
    PfsPartitionPicker().SelectionChanged({this, &MainWindow::on_pfs_partition_changed});
    FilesList().SelectionChanged({this, &MainWindow::on_files_selection_changed});
    FilesList().DoubleTapped({this, &MainWindow::on_files_double_tapped});
    FilesUpButton().Click({this, &MainWindow::on_files_up});
    ExportButton().Click({this, &MainWindow::on_export});
    MountButton().Click({this, &MainWindow::on_mount});
    MountPageMountButton().Click({this, &MainWindow::on_mount});
    OpenExplorerButton().Click({this, &MainWindow::on_open_explorer});
    MountPageExplorerButton().Click({this, &MainWindow::on_open_explorer});
    UnmountButton().Click({this, &MainWindow::on_unmount});
    ResetStatsButton().Click({this, &MainWindow::on_reset_stats});
    ThemeSelector().SelectionChanged({this, &MainWindow::on_theme_changed});
    RestartAdminInfoButton().Click({this, &MainWindow::on_restart_admin});
    RestartAdminButton().Click({this, &MainWindow::on_restart_admin});
    OpenLogButton().Click({this, &MainWindow::on_open_log});
    OpenLegacyButton().Click({this, &MainWindow::on_open_legacy});
    Closed({this, &MainWindow::on_closed});
}

void MainWindow::show_page(std::wstring_view tag, bool settings)
{
    HddPage().Visibility(Visibility::Collapsed);
    FilesPage().Visibility(Visibility::Collapsed);
    MountPage().Visibility(Visibility::Collapsed);
    PerformancePage().Visibility(Visibility::Collapsed);
    SettingsPage().Visibility(Visibility::Collapsed);

    if (settings) {
        SettingsPage().Visibility(Visibility::Visible);
    } else if (tag == L"files") {
        FilesPage().Visibility(Visibility::Visible);
        browse_current_pfs();
    } else if (tag == L"mount") {
        MountPage().Visibility(Visibility::Visible);
        poll_mount_state();
    } else if (tag == L"performance") {
        PerformancePage().Visibility(Visibility::Visible);
        refresh_performance();
    } else {
        HddPage().Visibility(Visibility::Visible);
    }
}

void MainWindow::on_navigation_invoked(NavigationView const&,
                                       NavigationViewItemInvokedEventArgs const& args)
{
    if (args.IsSettingsInvoked()) {
        show_page({}, true);
        return;
    }
    if (auto item = args.InvokedItemContainer().try_as<NavigationViewItem>()) {
        const auto tag = winrt::unbox_value_or<winrt::hstring>(item.Tag(), L"drive");
        show_page(tag.c_str(), false);
    }
}

void MainWindow::start_discovery(bool user_requested)
{
    if (discovery_thread_.joinable()) {
        discovery_thread_.request_stop();
        discovery_thread_.join();
    }
    set_status(L"Scanning Windows disk interfaces for PS2 APA volumes…");
    DriveSelector().Items().Clear();
    OpenDriveButton().IsEnabled(false);

    auto dispatcher = DispatcherQueue();
    auto weak = get_weak();
    discovery_thread_ = std::jthread([dispatcher, weak, user_requested](std::stop_token stop) {
        auto probes = ps2df::winui::NativeSessionController::discover_physical_drives();
        if (stop.stop_requested()) return;
        dispatcher.TryEnqueue([weak, probes = std::move(probes), user_requested]() mutable {
            if (auto self = weak.get()) {
                self->handle_discovery(std::move(probes), user_requested);
            }
        });
    });
}

void MainWindow::handle_discovery(std::vector<ps2hdd::PhysicalDriveProbe> probes, bool user_requested)
{
    detected_drives_.clear();
    bool access_denied = false;
    for (const auto& probe : probes) {
        access_denied = access_denied || probe.access_denied;
        if (probe.apa_detected) {
            detected_drives_.push_back(probe);
        }
    }

    DriveSelector().Items().Clear();
    for (const auto& probe : detected_drives_) {
        std::wstring label = L"PhysicalDrive" + std::to_wstring(probe.index);
        if (!probe.friendly_name.empty()) {
            label += L" — " + widen(probe.friendly_name);
        }
        label += L" · " + format_bytes(probe.size_bytes);
        DriveSelector().Items().Append(winrt::box_value(winrt::hstring(label)));
    }
    if (!detected_drives_.empty()) {
        DriveSelector().SelectedIndex(0);
        OpenDriveButton().IsEnabled(true);
    }

    AccessInfoBar().IsOpen(access_denied && detected_drives_.empty());

    if (detected_drives_.empty()) {
        set_status(access_denied ? L"Raw disk access denied — restart as Administrator or open an image"
                                 : L"No PS2 APA HDD detected — connect a disk or open an image");
        return;
    }

    if (!snapshot_.open && detected_drives_.size() == 1) {
        open_physical_async(detected_drives_.front().index);
        return;
    }

    std::wstring status = L"Detected " + std::to_wstring(detected_drives_.size()) + L" PS2 APA HDD";
    if (detected_drives_.size() != 1) status += L"s";
    status += L" — choose a source above";
    set_status(status);
    if (user_requested && detected_drives_.size() == 1) {
        open_physical_async(detected_drives_.front().index);
    }
}

void MainWindow::open_physical_async(unsigned index)
{
    if (source_busy_.exchange(true)) return;
    if (enrichment_thread_.joinable()) enrichment_thread_.request_stop();
    set_status(L"Opening PhysicalDrive" + std::to_wstring(index) + L" read-only…");

    auto dispatcher = DispatcherQueue();
    auto weak = get_weak();
    source_thread_ = std::jthread([this, dispatcher, weak, index](std::stop_token stop) {
        std::string error;
        const bool ok = !stop.stop_requested() && controller_.open_physical(index, error);
        dispatcher.TryEnqueue([weak, ok, error = std::move(error), index]() mutable {
            if (auto self = weak.get()) {
                self->finish_source_open(ok, std::move(error), index, {});
            }
        });
    });
}

void MainWindow::open_image_async(std::filesystem::path path)
{
    if (source_busy_.exchange(true)) return;
    if (enrichment_thread_.joinable()) enrichment_thread_.request_stop();
    set_status(L"Opening disk image read-only…");

    auto dispatcher = DispatcherQueue();
    auto weak = get_weak();
    source_thread_ = std::jthread([this, dispatcher, weak, path = std::move(path)](std::stop_token stop) mutable {
        std::string error;
        const bool ok = !stop.stop_requested() && controller_.open_image(path, error);
        dispatcher.TryEnqueue([weak, ok, error = std::move(error), path = std::move(path)]() mutable {
            if (auto self = weak.get()) {
                self->finish_source_open(ok, std::move(error), std::nullopt, std::move(path));
            }
        });
    });
}

void MainWindow::finish_source_open(bool ok, std::string error,
                                    std::optional<unsigned> physical,
                                    std::filesystem::path image)
{
    source_busy_.store(false);
    if (!ok) {
        set_status(L"Could not open source");
        show_error(L"PS2 DriveForge", widen(error));
        return;
    }

    current_physical_ = physical;
    current_image_ = std::move(image);
    AccessInfoBar().IsOpen(false);
    current_pfs_partition_.clear();
    current_pfs_path_.clear();
    refresh_all_from_snapshot();
    start_enrichment(false);
    MountButton().IsEnabled(true);
    MountPageMountButton().IsEnabled(true);
    set_status(L"Source opened read-only — APA catalog ready");
}

void MainWindow::refresh_all_from_snapshot()
{
    snapshot_ = controller_.snapshot();
    if (!snapshot_.open) return;

    SourceNameText().Text(htext(snapshot_.source_name));
    SourceSizeText().Text(format_bytes(snapshot_.source_size_bytes));
    SourceMediaText().Text(media_name(snapshot_.storage));
    SourceDetailText().Text(current_physical_ ? L"Physical PS2 HDD · GENERIC_READ" : L"Disk image · read-only file source");
    ApaRowsText().Text(std::to_wstring(snapshot_.rows.size()));
    HdlGamesText().Text(std::to_wstring(snapshot_.progress.total_hdl));
    BackingReadsText().Text(std::to_wstring(snapshot_.stats.backing_io.read_calls));

    MediaClassText().Text(L"Media: " + media_name(snapshot_.storage));
    BusTypeText().Text(L"Bus: " + bus_name(snapshot_.storage));
    SeekPenaltyText().Text(snapshot_.storage.seek_penalty_known
        ? std::wstring(L"Seek penalty: ") + (snapshot_.storage.incurs_seek_penalty ? L"Yes" : L"No")
        : L"Seek penalty: Unknown");
    TrimText().Text(snapshot_.storage.trim_known
        ? std::wstring(L"TRIM: ") + (snapshot_.storage.trim_enabled ? L"Enabled" : L"Disabled")
        : L"TRIM: Unknown");

    ApaScanTimeText().Text(L"APA scan: " + format_ms(snapshot_.stats.scan_time_ns));
    CatalogBuildTimeText().Text(L"Catalog build: " + format_ms(snapshot_.catalog_build_ms));
    const auto reads = snapshot_.stats.backing_io.read_calls;
    const double avg_ms = reads == 0 ? 0.0 :
        static_cast<double>(snapshot_.stats.backing_io.read_time_ns) / static_cast<double>(reads) / 1'000'000.0;
    AverageReadText().Text(reads == 0 ? L"Avg. backing read: —" : L"Avg. backing read: " + format_ms(avg_ms));

    refresh_partition_list();
    refresh_pfs_partitions();
    refresh_performance();
}

void MainWindow::refresh_partition_list()
{
    const std::string query = lower_ascii(winrt::to_string(FilterBox().Text()));
    const int kind_filter = PartitionKindFilter().SelectedIndex();
    const bool show_subs = ShowSubsToggle().IsOn();

    visible_partition_rows_.clear();
    PartitionList().Items().Clear();
    for (const auto& row : snapshot_.rows) {
        if (!show_subs && row.is_sub) continue;
        bool kind_ok = true;
        if (kind_filter == 1) kind_ok = row.kind == ps2hdd::PartitionCatalogKind::hdl;
        else if (kind_filter == 2) kind_ok = row.kind == ps2hdd::PartitionCatalogKind::pfs;
        else if (kind_filter == 3) kind_ok = row.kind != ps2hdd::PartitionCatalogKind::hdl && row.kind != ps2hdd::PartitionCatalogKind::pfs;
        if (!kind_ok) continue;

        if (!query.empty()) {
            const auto haystack = lower_ascii(row.title + " " + row.partition_id + " " + row.startup);
            if (haystack.find(query) == std::string::npos) continue;
        }
        visible_partition_rows_.push_back(row);
        PartitionList().Items().Append(make_partition_row(row));
    }
    EmptyState().Visibility(snapshot_.open && !visible_partition_rows_.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void MainWindow::refresh_pfs_partitions()
{
    const auto old = current_pfs_partition_;
    pfs_partitions_.clear();
    for (const auto& row : snapshot_.rows) {
        if (!row.is_sub && row.kind == ps2hdd::PartitionCatalogKind::pfs) {
            if (std::find(pfs_partitions_.begin(), pfs_partitions_.end(), row.partition_id) == pfs_partitions_.end()) {
                pfs_partitions_.push_back(row.partition_id);
            }
        }
    }

    PfsPartitionPicker().Items().Clear();
    int selected = -1;
    for (std::size_t i = 0; i < pfs_partitions_.size(); ++i) {
        PfsPartitionPicker().Items().Append(winrt::box_value(htext(pfs_partitions_[i])));
        if (pfs_partitions_[i] == old) selected = static_cast<int>(i);
    }
    if (selected < 0 && !pfs_partitions_.empty()) selected = 0;
    if (selected >= 0) {
        current_pfs_partition_ = pfs_partitions_[static_cast<std::size_t>(selected)];
        PfsPartitionPicker().SelectedIndex(selected);
    } else {
        current_pfs_partition_.clear();
        visible_file_entries_.clear();
        FilesList().Items().Clear();
    }
}

void MainWindow::start_enrichment(bool reset)
{
    if (!snapshot_.open) return;
    if (enrichment_thread_.joinable()) {
        enrichment_thread_.request_stop();
        enrichment_thread_.join();
    }
    if (reset) {
        controller_.reset_enrichment();
        snapshot_ = controller_.snapshot();
        refresh_partition_list();
    }
    if (snapshot_.progress.total_hdl == 0) {
        EnrichmentStateText().Text(L"no HDL partitions");
        EnrichmentProgressText().Text(L"Nothing to enrich");
        EnrichmentProgress().Value(0);
        CancelEnrichmentButton().IsEnabled(false);
        return;
    }

    CancelEnrichmentButton().IsEnabled(true);
    EnrichmentStateText().Text(L"metadata loading");
    EnrichmentProgress().Maximum(static_cast<double>(snapshot_.progress.total_hdl));
    EnrichmentProgress().Value(0);

    auto dispatcher = DispatcherQueue();
    auto weak = get_weak();
    enrichment_thread_ = std::jthread([this, dispatcher, weak](std::stop_token stop) {
        controller_.enrich_hdl([dispatcher, weak](std::size_t completed, std::size_t total) {
            dispatcher.TryEnqueue([weak, completed, total] {
                if (auto self = weak.get()) {
                    self->snapshot_ = self->controller_.snapshot();
                    self->EnrichmentProgress().Maximum(static_cast<double>(std::max<std::size_t>(1, total)));
                    self->EnrichmentProgress().Value(static_cast<double>(completed));
                    self->EnrichmentProgressText().Text(std::to_wstring(completed) + L" / " + std::to_wstring(total));
                    self->HdlGamesText().Text(std::to_wstring(self->snapshot_.progress.ready_hdl));
                    self->BackingReadsText().Text(std::to_wstring(self->snapshot_.stats.backing_io.read_calls));
                    self->refresh_partition_list();
                }
            });
        }, stop);
        dispatcher.TryEnqueue([weak, cancelled = stop.stop_requested()] {
            if (auto self = weak.get()) {
                self->snapshot_ = self->controller_.snapshot();
                self->CancelEnrichmentButton().IsEnabled(false);
                self->EnrichmentStateText().Text(cancelled ? L"metadata cancelled" : L"metadata ready");
                self->EnrichmentProgressText().Text(cancelled ? L"Cancelled" : L"Complete");
                self->refresh_partition_list();
                self->refresh_performance();
            }
        });
    });
}

void MainWindow::browse_current_pfs()
{
    if (!snapshot_.open || current_pfs_partition_.empty()) return;
    const auto result = controller_.browse_pfs(current_pfs_partition_, current_pfs_path_);
    if (!result.ok) {
        show_error(L"PFS browser", widen(result.error));
        return;
    }
    visible_file_entries_ = result.entries;
    refresh_files_list();
}

void MainWindow::refresh_files_list()
{
    FilesList().Items().Clear();
    for (const auto& entry : visible_file_entries_) {
        FilesList().Items().Append(make_file_row(entry));
    }
    FilesPathText().Text(current_pfs_path_.empty() ? L"/" : L"/" + widen(current_pfs_path_));
    FilesUpButton().IsEnabled(!current_pfs_path_.empty());
    ExportButton().IsEnabled(false);
}

void MainWindow::export_selected_entry()
{
    const int selected = FilesList().SelectedIndex();
    if (selected < 0 || static_cast<std::size_t>(selected) >= visible_file_entries_.size()) return;
    const auto entry = visible_file_entries_[static_cast<std::size_t>(selected)];

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        show_error(L"Export", L"Could not create the destination folder picker.");
        return;
    }
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    hr = dialog->Show(native_hwnd());
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        return;
    }
    IShellItem* item = nullptr;
    if (FAILED(hr) || FAILED(dialog->GetResult(&item)) || !item) {
        dialog->Release();
        show_error(L"Export", L"Could not read the selected destination folder.");
        return;
    }
    PWSTR raw_path = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path);
    item->Release();
    dialog->Release();
    if (FAILED(hr) || !raw_path) {
        show_error(L"Export", L"The selected destination does not have a filesystem path.");
        return;
    }
    std::filesystem::path destination(raw_path);
    CoTaskMemFree(raw_path);

    const auto source_path = join_pfs_path(current_pfs_path_, entry.name);
    destination /= widen(ps2hdd::pfs::sanitize_host_filename(entry.name));
    set_status(L"Exporting " + widen(entry.name) + L"…");

    auto dispatcher = DispatcherQueue();
    auto weak = get_weak();
    const auto partition = current_pfs_partition_;
    export_thread_ = std::jthread([this, dispatcher, weak, partition, source_path, destination](std::stop_token) {
        std::string error;
        const bool ok = controller_.export_to_host(partition, source_path, destination, error);
        dispatcher.TryEnqueue([weak, ok, error = std::move(error), destination] {
            if (auto self = weak.get()) {
                if (ok) {
                    self->set_status(L"Export complete: " + destination.wstring());
                } else {
                    self->set_status(L"Export failed");
                    self->show_error(L"Export", widen(error));
                }
                self->refresh_performance();
            }
        });
    });
}

void MainWindow::refresh_performance()
{
    if (!snapshot_.open) {
        PerfSourceText().Text(L"No source");
        return;
    }
    snapshot_ = controller_.snapshot();
    const auto& stats = snapshot_.stats;
    const auto reads = stats.backing_io.read_calls;
    const double avg_ms = reads == 0 ? 0.0 : static_cast<double>(stats.backing_io.read_time_ns) / reads / 1'000'000.0;
    PerfSourceText().Text(htext(snapshot_.source_name));
    PerfApaText().Text(L"APA scan: " + format_ms(stats.scan_time_ns));
    PerfCatalogText().Text(L"Catalog build: " + format_ms(snapshot_.catalog_build_ms));
    PerfReadsText().Text(L"Backing reads: " + std::to_wstring(reads));
    PerfBytesText().Text(L"Backing bytes: " + format_bytes(stats.backing_io.bytes_requested));
    PerfAvgText().Text(reads == 0 ? L"Average backing read: —" : L"Average backing read: " + format_ms(avg_ms));
    PerfCacheText().Text(L"Cache hits/misses — browse " + std::to_wstring(stats.cache.browse_hits) + L"/" +
                        std::to_wstring(stats.cache.browse_misses) + L", stat " +
                        std::to_wstring(stats.cache.stat_hits) + L"/" + std::to_wstring(stats.cache.stat_misses));
}

void MainWindow::start_mount()
{
    if (!snapshot_.open) {
        show_error(L"Mount", L"Open a PS2 HDD or disk image first.");
        return;
    }
    if (!mount_point_.empty()) {
        poll_mount_state();
        if (!mount_point_.empty()) return;
    }

    const auto helper = mount_helper_path();
    if (!path_exists(helper)) {
        show_error(L"Mount", L"PS2-DriveForge-Mount.exe is not present. Use the full Setup/Portable package rather than the standalone WinUI development artifact.");
        return;
    }
    const auto letter = ps2hdd::darkness_policy::choose_mount_letter(GetLogicalDrives());
    if (!letter) {
        show_error(L"Mount", L"No free drive letter from C: through Z: is available.");
        return;
    }
    mount_point_ = std::wstring{*letter, L':', L'\\'};

    std::wstring parameters;
    bool elevate = false;
    if (current_physical_) {
        parameters = L"--physical " + std::to_wstring(*current_physical_) + L" --mount \"" + mount_point_ + L"\"";
        elevate = true;
    } else if (!current_image_.empty()) {
        parameters = L"--image \"" + current_image_.wstring() + L"\" --mount \"" + mount_point_ + L"\"";
    } else {
        mount_point_.clear();
        show_error(L"Mount", L"The current source cannot be reopened for mounting.");
        return;
    }

    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = native_hwnd();
    execute.lpVerb = elevate ? L"runas" : L"open";
    execute.lpFile = helper.c_str();
    execute.lpParameters = parameters.c_str();
    const auto root = package_root();
    execute.lpDirectory = root.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute) || !execute.hProcess) {
        const DWORD error = GetLastError();
        mount_point_.clear();
        if (error != ERROR_CANCELLED) show_error(L"Mount", L"Could not start the read-only mount helper.");
        return;
    }
    if (mount_process_) CloseHandle(mount_process_);
    mount_process_ = execute.hProcess;
    MountStatusText().Text(L"Mounting read-only…");
    MountPointText().Text(L"Drive letter: " + mount_point_);
    set_status(L"Mounting DriveForge read-only at " + mount_point_ + L"…");
    mount_timer_.Start();
    poll_mount_state();
}

void MainWindow::request_unmount(bool interactive)
{
    if (mount_point_.empty()) return;
    const auto helper = mount_helper_path();
    if (!path_exists(helper)) return;
    std::wstring parameters = L"--unmount \"" + mount_point_ + L"\"";
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = native_hwnd();
    execute.lpVerb = L"runas";
    execute.lpFile = helper.c_str();
    execute.lpParameters = parameters.c_str();
    const auto root = package_root();
    execute.lpDirectory = root.c_str();
    execute.nShow = SW_HIDE;
    if (ShellExecuteExW(&execute)) {
        if (execute.hProcess) CloseHandle(execute.hProcess);
        set_status(L"Unmount requested for " + mount_point_);
        MountStatusText().Text(L"Unmounting…");
        mount_timer_.Start();
    } else if (interactive && GetLastError() != ERROR_CANCELLED) {
        show_error(L"Unmount", L"Could not start the unmount helper.");
    }
}

void MainWindow::poll_mount_state()
{
    if (mount_point_.empty()) {
        MountStatusText().Text(L"Nothing mounted");
        MountPointText().Text(L"Drive letter: automatic");
        OpenExplorerButton().IsEnabled(false);
        MountPageExplorerButton().IsEnabled(false);
        UnmountButton().IsEnabled(false);
        MountButton().IsEnabled(snapshot_.open);
        MountPageMountButton().IsEnabled(snapshot_.open);
        return;
    }

    const bool mounted = GetFileAttributesW(mount_point_.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (mounted) {
        MountStatusText().Text(L"Mounted read-only");
        MountPointText().Text(L"Drive letter: " + mount_point_);
        OpenExplorerButton().IsEnabled(true);
        MountPageExplorerButton().IsEnabled(true);
        UnmountButton().IsEnabled(true);
        MountButton().IsEnabled(false);
        MountPageMountButton().IsEnabled(false);
        set_status(L"Mounted read-only at " + mount_point_);
        return;
    }

    if (mount_process_ && WaitForSingleObject(mount_process_, 0) == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(mount_process_, &exit_code);
        CloseHandle(mount_process_);
        mount_process_ = nullptr;
        const auto old = mount_point_;
        mount_point_.clear();
        mount_timer_.Stop();
        MountStatusText().Text(exit_code == 0 ? L"Nothing mounted" : L"Mount helper exited with an error");
        MountPointText().Text(L"Drive letter: automatic");
        OpenExplorerButton().IsEnabled(false);
        MountPageExplorerButton().IsEnabled(false);
        UnmountButton().IsEnabled(false);
        MountButton().IsEnabled(snapshot_.open);
        MountPageMountButton().IsEnabled(snapshot_.open);
        set_status(exit_code == 0 ? L"Unmounted " + old : L"Mount failed — helper exit code " + std::to_wstring(exit_code));
    }
}

void MainWindow::open_mounted_volume()
{
    if (!mount_point_.empty()) {
        ShellExecuteW(native_hwnd(), L"open", mount_point_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void MainWindow::load_theme_preference()
{
    int index = 0;
    std::ifstream input(settings_path());
    std::string value;
    if (input >> value) {
        if (value == "light") index = 1;
        else if (value == "dark") index = 2;
    }
    suppress_theme_event_ = true;
    ThemeSelector().SelectedIndex(index);
    suppress_theme_event_ = false;
    apply_theme_index(index);
}

void MainWindow::save_theme_preference(int index)
{
    const auto path = settings_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream output(path, std::ios::trunc);
    output << (index == 1 ? "light" : (index == 2 ? "dark" : "system"));
}

void MainWindow::apply_theme_index(int index)
{
    RootGrid().RequestedTheme(index == 1 ? ElementTheme::Light : (index == 2 ? ElementTheme::Dark : ElementTheme::Default));
}

void MainWindow::restart_as_administrator()
{
    auto target = launcher_path();
    if (!path_exists(target)) target = executable_path();
    const auto directory = target.parent_path();
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(native_hwnd(), L"runas", target.c_str(), nullptr,
                                                                directory.c_str(), SW_SHOWNORMAL));
    if (result > 32) {
        Close();
    } else if (result != SE_ERR_ACCESSDENIED) {
        show_error(L"Administrator restart", L"Windows could not restart DriveForge with elevated privileges.");
    }
}

void MainWindow::open_startup_log()
{
    const auto path = startup_log_path();
    if (!path_exists(path)) {
        show_error(L"Startup log", L"No WinUI startup log has been created yet.");
        return;
    }
    ShellExecuteW(native_hwnd(), L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::open_legacy_frontend()
{
    const auto legacy = package_root() / L"legacy" / L"PS2-DriveForge-Win32.exe";
    if (!path_exists(legacy)) {
        show_error(L"Legacy Win32", L"The Legacy Win32 frontend is not present in this package.");
        return;
    }
    const auto root = package_root();
    ShellExecuteW(native_hwnd(), L"open", legacy.c_str(), nullptr, root.c_str(), SW_SHOWNORMAL);
}

void MainWindow::show_error(std::wstring_view title, std::wstring_view message) const
{
    MessageBoxW(native_hwnd(), std::wstring(message).c_str(), std::wstring(title).c_str(), MB_OK | MB_ICONERROR);
}

void MainWindow::set_status(std::wstring_view text)
{
    StatusText().Text(std::wstring(text));
}

HWND MainWindow::native_hwnd() const
{
    HWND handle{};
    auto native = const_cast<MainWindow*>(this)->try_as<::IWindowNative>();
    if (native) winrt::check_hresult(native->get_WindowHandle(&handle));
    return handle;
}

std::filesystem::path MainWindow::executable_path() const
{
    std::array<wchar_t, 32768> path{};
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring(path.data(), count));
}

std::filesystem::path MainWindow::package_root() const
{
    auto path = executable_path().parent_path();
    if (path.filename() == L"winui") path = path.parent_path();
    if (path.filename() == L"app") path = path.parent_path();
    return path;
}

std::filesystem::path MainWindow::mount_helper_path() const
{
    return package_root() / L"tools" / L"PS2-DriveForge-Mount.exe";
}

std::filesystem::path MainWindow::launcher_path() const
{
    return package_root() / L"PS2-DriveForge.exe";
}

std::filesystem::path MainWindow::startup_log_path() const
{
    return local_app_data_path() / L"PS2 DriveForge" / L"Logs" / L"winui-startup.log";
}

std::filesystem::path MainWindow::settings_path() const
{
    return local_app_data_path() / L"PS2 DriveForge" / L"settings.txt";
}

void MainWindow::on_open_image(IInspectable const&, RoutedEventArgs const&)
{
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = native_hwnd();
    dialog.lpstrFilter = L"Disk images (*.img;*.bin;*.raw)\0*.img;*.bin;*.raw\0All files\0*.*\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        open_image_async(std::filesystem::path(path.data()));
    }
}

void MainWindow::on_rescan(IInspectable const&, RoutedEventArgs const&) { start_discovery(true); }

void MainWindow::on_open_drive(IInspectable const&, RoutedEventArgs const&)
{
    const int selected = DriveSelector().SelectedIndex();
    if (selected >= 0 && static_cast<std::size_t>(selected) < detected_drives_.size()) {
        open_physical_async(detected_drives_[static_cast<std::size_t>(selected)].index);
    }
}

void MainWindow::on_filter_changed(IInspectable const&, TextChangedEventArgs const&) { refresh_partition_list(); }
void MainWindow::on_partition_filter_changed(IInspectable const&, SelectionChangedEventArgs const&) { refresh_partition_list(); }
void MainWindow::on_show_subs_toggled(IInspectable const&, RoutedEventArgs const&) { refresh_partition_list(); }
void MainWindow::on_refresh_metadata(IInspectable const&, RoutedEventArgs const&) { start_enrichment(true); }
void MainWindow::on_cancel_enrichment(IInspectable const&, RoutedEventArgs const&) { if (enrichment_thread_.joinable()) enrichment_thread_.request_stop(); }

void MainWindow::on_pfs_partition_changed(IInspectable const&, SelectionChangedEventArgs const&)
{
    const int selected = PfsPartitionPicker().SelectedIndex();
    if (selected < 0 || static_cast<std::size_t>(selected) >= pfs_partitions_.size()) return;
    current_pfs_partition_ = pfs_partitions_[static_cast<std::size_t>(selected)];
    current_pfs_path_.clear();
    browse_current_pfs();
}

void MainWindow::on_files_selection_changed(IInspectable const&, SelectionChangedEventArgs const&)
{
    const int selected = FilesList().SelectedIndex();
    ExportButton().IsEnabled(selected >= 0 && static_cast<std::size_t>(selected) < visible_file_entries_.size());
}

void MainWindow::on_files_double_tapped(IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&)
{
    const int selected = FilesList().SelectedIndex();
    if (selected < 0 || static_cast<std::size_t>(selected) >= visible_file_entries_.size()) return;
    const auto& entry = visible_file_entries_[static_cast<std::size_t>(selected)];
    if (entry.is_directory) {
        current_pfs_path_ = join_pfs_path(current_pfs_path_, entry.name);
        browse_current_pfs();
    }
}

void MainWindow::on_files_up(IInspectable const&, RoutedEventArgs const&)
{
    current_pfs_path_ = parent_pfs_path(current_pfs_path_);
    browse_current_pfs();
}
void MainWindow::on_export(IInspectable const&, RoutedEventArgs const&) { export_selected_entry(); }
void MainWindow::on_mount(IInspectable const&, RoutedEventArgs const&) { start_mount(); }
void MainWindow::on_open_explorer(IInspectable const&, RoutedEventArgs const&) { open_mounted_volume(); }
void MainWindow::on_unmount(IInspectable const&, RoutedEventArgs const&) { request_unmount(true); }

void MainWindow::on_reset_stats(IInspectable const&, RoutedEventArgs const&)
{
    controller_.reset_stats();
    snapshot_ = controller_.snapshot();
    refresh_all_from_snapshot();
    set_status(L"Performance counters reset");
}

void MainWindow::on_theme_changed(IInspectable const&, SelectionChangedEventArgs const&)
{
    if (suppress_theme_event_) return;
    const int index = std::max(0, ThemeSelector().SelectedIndex());
    apply_theme_index(index);
    save_theme_preference(index);
}

void MainWindow::on_restart_admin(IInspectable const&, RoutedEventArgs const&) { restart_as_administrator(); }
void MainWindow::on_open_log(IInspectable const&, RoutedEventArgs const&) { open_startup_log(); }
void MainWindow::on_open_legacy(IInspectable const&, RoutedEventArgs const&) { open_legacy_frontend(); }

void MainWindow::on_telemetry_tick(IInspectable const&, IInspectable const&)
{
    if (snapshot_.open) {
        snapshot_ = controller_.snapshot();
        BackingReadsText().Text(std::to_wstring(snapshot_.stats.backing_io.read_calls));
        refresh_performance();
    }
}

void MainWindow::on_mount_tick(IInspectable const&, IInspectable const&) { poll_mount_state(); }

void MainWindow::on_closed(Microsoft::UI::Xaml::Window const&, Microsoft::UI::Xaml::WindowEventArgs const&)
{
    if (!mount_point_.empty()) request_unmount(false);
    if (telemetry_timer_) telemetry_timer_.Stop();
    if (mount_timer_) mount_timer_.Stop();
}

} // namespace winrt::PS2DriveForge::WinUI::implementation
