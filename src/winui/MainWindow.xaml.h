#pragma once

#include "pch.h"
#include "MainWindow.g.h"
#include "NativeSessionController.hpp"
#include "WinUIRecoveryActions.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace winrt::PS2DriveForge::WinUI::implementation
{
struct MainWindow : MainWindowT<MainWindow>
{
    MainWindow();
    ~MainWindow();

    // XAML code generation binds these handlers from MainWindow.xaml and must
    // be able to take their addresses. Keep only the declarative recovery event
    // surface public; storage helpers and state remain private.
    void on_capture_rescue(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (mutation_busy_.load()) return;
        if (!current_physical_) {
            show_error(L"Create Rescue Capsule", L"Open a physical PS2 HDD first. Disk images do not have a live FHDB bootstrap pointer to capture.");
            return;
        }
        if (hdl_artifact_directory_.empty()) {
            show_error(L"Create Rescue Capsule", L"Choose a safety/artifact directory first.");
            return;
        }
        const auto result = controller_.capture_rescue_capsule(hdl_artifact_directory_);
        if (!result.ok) {
            show_error(L"Create Rescue Capsule", std::wstring(result.error.begin(), result.error.end()));
            return;
        }
        show_recovery_success(L"Rescue Capsule created", result.artifact_path.wstring());
        set_status(L"Canonical PS2HBRC Rescue Capsule captured read-only");
    }

    void on_restore_rescue(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (mutation_busy_.load()) return;
        if (!current_physical_) {
            show_error(L"Restore Rescue Capsule", L"Bootstrap restore is available only for a physical PS2 HDD.");
            return;
        }
        if (hdl_artifact_directory_.empty()) {
            show_error(L"Restore Rescue Capsule", L"Choose the directory containing HDDRESCUE/HDDMBR/FHDBMBR inputs first.");
            return;
        }
        confirm_physical_action(L"restore bootstrap/recovery artifacts", [this] {
            mutation_busy_.store(true);
            const auto result = controller_.restore_bootstrap(hdl_artifact_directory_, hdl_artifact_directory_, true);
            mutation_busy_.store(false);
            if (!result.ok) {
                show_error(L"Restore Rescue Capsule", std::wstring(result.error.begin(), result.error.end()));
                return;
            }
            show_recovery_success(
                L"Bootstrap restored",
                result.cold_payload_verified
                    ? L"Payload and master pointer were cold-verified through a fresh read-only PhysicalDrive."
                    : L"Master pointer was cold-verified through a fresh read-only PhysicalDrive.");
            refresh_all_from_snapshot();
            refresh_hdl_tools();
            set_status(L"Bootstrap recovery completed and cold-verified");
        });
    }

    void on_forensic_analyze(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!current_physical_) {
            show_error(L"Forensic APA analysis", L"Open a physical PS2 HDD first.");
            return;
        }
        if (hdl_artifact_directory_.empty()) {
            show_error(L"Forensic APA analysis", L"Choose a safety/artifact directory first.");
            return;
        }
        const auto result = ps2df::winui::analyze_physical_apa(*current_physical_, hdl_artifact_directory_);
        if (!result.ok) {
            show_error(L"Forensic APA analysis", std::wstring(result.error.begin(), result.error.end()));
            return;
        }
        std::wstring message = L"FORENSIC.TXT: " + result.report_path.wstring();
        if (result.automatic_safe) {
            message += L"\nAutomatic-safe topology repair evidence is available.";
        } else {
            message += L"\nNo automatic-safe topology repair was found; no write is offered.";
        }
        show_recovery_success(L"Forensic analysis complete", message);
        set_status(L"Forensic APA analysis completed read-only");
    }

    void on_repair_master(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!current_physical_ || hdl_artifact_directory_.empty()) {
            show_error(L"Repair APA master", L"Open a physical PS2 HDD and choose a safety/artifact directory first.");
            return;
        }
        confirm_physical_action(L"apply conservative APA master repair", [this] {
            mutation_busy_.store(true);
            const auto result = ps2df::winui::repair_physical_master(*current_physical_, hdl_artifact_directory_);
            mutation_busy_.store(false);
            if (!result.ok) {
                show_error(L"Repair APA master", std::wstring(result.error.begin(), result.error.end()));
                return;
            }
            show_recovery_success(L"APA master repaired", L"The conservative repair was cold-verified through a fresh read-only scan.");
            std::string reopen_error;
            if (!controller_.open_physical(*current_physical_, reopen_error)) {
                show_error(L"Repair APA master", L"Repair committed, but the WinUI read-only session could not reopen the disk.");
                return;
            }
            refresh_all_from_snapshot();
            refresh_hdl_tools();
        });
    }

    void on_repair_forensic(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!current_physical_ || hdl_artifact_directory_.empty()) {
            show_error(L"Forensic APA repair", L"Open a physical PS2 HDD and choose a safety/artifact directory first.");
            return;
        }
        confirm_physical_action(L"apply automatic-safe forensic APA topology repair", [this] {
            mutation_busy_.store(true);
            const auto result = ps2df::winui::repair_physical_apa_automatic(*current_physical_, hdl_artifact_directory_);
            mutation_busy_.store(false);
            if (!result.ok) {
                show_error(L"Forensic APA repair", std::wstring(result.error.begin(), result.error.end()));
                return;
            }
            show_recovery_success(L"Forensic APA repair complete", L"Only an automatic-safe evidence map was accepted; touched headers and the final APA scan were cold-verified.");
            std::string reopen_error;
            if (!controller_.open_physical(*current_physical_, reopen_error)) {
                show_error(L"Forensic APA repair", L"Repair committed, but the WinUI read-only session could not reopen the disk.");
                return;
            }
            refresh_all_from_snapshot();
            refresh_hdl_tools();
        });
    }

private:
    void wire_events();
    void load_theme_preference();
    void save_theme_preference(int index);
    void apply_theme_index(int index);
    void show_page(std::wstring_view tag, bool settings = false);

    void start_discovery(bool user_requested);
    void handle_discovery(std::vector<ps2hdd::PhysicalDriveProbe> probes, bool user_requested);
    void open_physical_async(unsigned index);
    void open_image_async(std::filesystem::path path);
    void finish_source_open(bool ok, std::string error,
                            std::optional<unsigned> physical,
                            std::filesystem::path image);

    void refresh_all_from_snapshot();
    void refresh_partition_list();
    void refresh_pfs_partitions();
    void refresh_performance();
    void start_enrichment(bool reset);

    void refresh_hdl_tools();
    void choose_hdl_iso();
    void choose_hdl_artifact_directory();
    void preview_hdl_install();
    void start_hdl_install(bool physical_confirmed);
    void start_hdl_remove(bool physical_confirmed);
    void run_physical_preflight();
    void confirm_physical_action(std::wstring_view action,
                                 std::function<void()> continuation);
    [[nodiscard]] ps2df::winui::HdlInstallRequest current_hdl_request() const;

    void browse_current_pfs();
    void refresh_files_list();
    void export_selected_entry();

    void start_mount();
    void request_unmount(bool interactive = true);
    void poll_mount_state();
    void open_mounted_volume();

    void restart_as_administrator();
    void open_startup_log();
    void open_legacy_frontend();
    void show_error(std::wstring_view title, std::wstring_view message) const;
    void set_status(std::wstring_view text);

    [[nodiscard]] HWND native_hwnd() const;
    [[nodiscard]] std::filesystem::path executable_path() const;
    [[nodiscard]] std::filesystem::path package_root() const;
    [[nodiscard]] std::filesystem::path mount_helper_path() const;
    [[nodiscard]] std::filesystem::path launcher_path() const;
    [[nodiscard]] std::filesystem::path startup_log_path() const;
    [[nodiscard]] std::filesystem::path settings_path() const;

    void on_navigation_invoked(Microsoft::UI::Xaml::Controls::NavigationView const&,
                               Microsoft::UI::Xaml::Controls::NavigationViewItemInvokedEventArgs const& args);
    void on_open_image(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_rescan(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_open_drive(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_filter_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void on_partition_filter_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_show_subs_toggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_refresh_metadata(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_cancel_enrichment(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    void on_hdl_choose_iso(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_hdl_choose_artifacts(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_hdl_preview(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_hdl_install(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_hdl_game_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_hdl_delete(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_physical_preflight(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    void show_recovery_success(std::wstring_view title, std::wstring_view message)
    {
        HdlToolsInfoBar().Severity(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
        HdlToolsInfoBar().Title(title);
        HdlToolsInfoBar().Message(message);
        HdlToolsInfoBar().IsOpen(true);
    }

    void on_pfs_partition_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_files_selection_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_files_double_tapped(IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
    void on_files_up(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_export(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_mount(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_open_explorer(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_unmount(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_reset_stats(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_theme_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_restart_admin(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_open_log(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_open_legacy(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_telemetry_tick(IInspectable const&, IInspectable const&);
    void on_mount_tick(IInspectable const&, IInspectable const&);
    void on_closed(IInspectable const&, Microsoft::UI::Xaml::WindowEventArgs const&);

    ps2df::winui::NativeSessionController controller_;
    ps2df::winui::SessionSnapshot snapshot_;
    std::vector<ps2hdd::PhysicalDriveProbe> detected_drives_;
    std::vector<ps2df::winui::PartitionRowSnapshot> visible_partition_rows_;
    std::vector<ps2df::winui::PartitionRowSnapshot> hdl_game_rows_;
    std::vector<std::string> pfs_partitions_;
    std::vector<ps2df::winui::BrowseEntrySnapshot> visible_file_entries_;

    std::optional<unsigned> current_physical_;
    std::filesystem::path current_image_;
    std::string current_pfs_partition_;
    std::string current_pfs_path_;

    std::filesystem::path hdl_iso_path_;
    std::filesystem::path hdl_artifact_directory_;
    ps2df::winui::HdlInstallPreviewSnapshot hdl_preview_;

    std::jthread discovery_thread_;
    std::jthread source_thread_;
    std::jthread enrichment_thread_;
    std::jthread export_thread_;
    std::jthread mutation_thread_;
    std::atomic_bool source_busy_{false};
    std::atomic_bool mutation_busy_{false};
    bool suppress_theme_event_{false};

    Microsoft::UI::Xaml::DispatcherTimer telemetry_timer_;
    Microsoft::UI::Xaml::DispatcherTimer mount_timer_;
    HANDLE mount_process_{nullptr};
    std::wstring mount_point_;
};
}

namespace winrt::PS2DriveForge::WinUI::factory_implementation
{
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
{
};
}
