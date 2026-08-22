#pragma once

#include "pch.h"
#include "MainWindow.g.h"
#include "NativeSessionController.hpp"

#include <atomic>
#include <filesystem>
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
    std::vector<std::string> pfs_partitions_;
    std::vector<ps2df::winui::BrowseEntrySnapshot> visible_file_entries_;

    std::optional<unsigned> current_physical_;
    std::filesystem::path current_image_;
    std::string current_pfs_partition_;
    std::string current_pfs_path_;

    std::jthread discovery_thread_;
    std::jthread source_thread_;
    std::jthread enrichment_thread_;
    std::jthread export_thread_;
    std::atomic_bool source_busy_{false};
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
