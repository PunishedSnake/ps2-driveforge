#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "StartupLog.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>

namespace ps2df::winui::diag {
namespace {
std::once_flag g_initialize_once;
std::mutex g_write_mutex;
std::wstring g_log_path;

std::string utf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }

    const auto size = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "<UTF-8 conversion failed>";
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    const auto written = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    if (written != size) {
        return "<UTF-8 conversion failed>";
    }
    return result;
}

std::wstring make_log_path()
{
    wchar_t local_app_data[32768]{};
    const auto capacity = static_cast<DWORD>(_countof(local_app_data));
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, capacity);

    std::wstring root;
    if (length > 0 && length < capacity) {
        root.assign(local_app_data, length);
    } else {
        root = L".";
    }

    root += L"\\PS2 DriveForge";
    (void)CreateDirectoryW(root.c_str(), nullptr);
    root += L"\\Logs";
    (void)CreateDirectoryW(root.c_str(), nullptr);
    root += L"\\winui-startup.log";
    return root;
}

void write_line(std::string_view message) noexcept
{
    try {
        SYSTEMTIME now{};
        GetLocalTime(&now);

        char prefix[160]{};
        std::snprintf(
            prefix,
            sizeof(prefix),
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] pid=%lu tid=%lu ",
            now.wYear,
            now.wMonth,
            now.wDay,
            now.wHour,
            now.wMinute,
            now.wSecond,
            now.wMilliseconds,
            static_cast<unsigned long>(GetCurrentProcessId()),
            static_cast<unsigned long>(GetCurrentThreadId()));

        std::string line(prefix);
        line.append(message.data(), message.size());
        line.append("\r\n");

        OutputDebugStringA(line.c_str());
        if (g_log_path.empty()) {
            return;
        }

        std::scoped_lock lock(g_write_mutex);
        const auto file = CreateFileW(
            g_log_path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }

        DWORD written{};
        (void)WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        (void)FlushFileBuffers(file);
        (void)CloseHandle(file);
    } catch (...) {
        // Diagnostics must never become a new reason for the frontend to fail.
    }
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) noexcept
{
    char buffer[192]{};
    if (info && info->ExceptionRecord) {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "native unhandled exception: code=0x%08lX address=%p",
            static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
            info->ExceptionRecord->ExceptionAddress);
    } else {
        std::snprintf(buffer, sizeof(buffer), "native unhandled exception: no EXCEPTION_POINTERS");
    }
    write_line(buffer);
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminate_handler() noexcept
{
    write_line("std::terminate invoked");
    std::abort();
}

void signal_launcher_ready() noexcept
{
    try {
        wchar_t event_name[512]{};
        const DWORD length = GetEnvironmentVariableW(
            L"PS2DF_WINUI_READY_EVENT", event_name, static_cast<DWORD>(_countof(event_name)));
        if (length == 0 || length >= _countof(event_name)) {
            write_line("launcher readiness event not present (direct WinUI launch)");
            return;
        }

        const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name);
        if (!event) {
            write_line("launcher readiness event could not be opened");
            return;
        }

        if (SetEvent(event)) {
            write_line("launcher readiness event signaled");
        } else {
            write_line("launcher readiness event signaling failed");
        }
        CloseHandle(event);
    } catch (...) {
        write_line("launcher readiness signaling threw unexpectedly");
    }
}
} // namespace

void initialize() noexcept
{
    try {
        std::call_once(g_initialize_once, [] {
            g_log_path = make_log_path();
            (void)SetUnhandledExceptionFilter(unhandled_exception_filter);
            std::set_terminate(terminate_handler);
            write_line("WinUI diagnostics initialized");

            try {
                wchar_t module_path[32768]{};
                const auto length = GetModuleFileNameW(
                    nullptr, module_path, static_cast<DWORD>(_countof(module_path)));
                if (length > 0 && length < _countof(module_path)) {
                    write_line(std::string("executable=") + utf8(std::wstring_view(module_path, length)));
                }
                write_line(std::string("log_path=") + utf8(g_log_path));
            } catch (...) {
                write_line("failed to format bootstrap path diagnostics");
            }
        });
    } catch (...) {
        // Best-effort only. Startup must continue even if logging cannot initialize.
    }
}

void log(std::string_view message) noexcept
{
    initialize();
    write_line(message);
}

void log_hresult(std::string_view context, long code, std::wstring_view message) noexcept
{
    try {
        char code_text[32]{};
        std::snprintf(code_text, sizeof(code_text), "0x%08lX", static_cast<unsigned long>(code));

        std::string line(context);
        line += " HRESULT=";
        line += code_text;
        if (!message.empty()) {
            line += " message=\"";
            line += utf8(message);
            line += '"';
        }
        log(line);
    } catch (...) {
        write_line("failed while formatting HRESULT diagnostics");
    }
}

std::wstring log_path() noexcept
{
    try {
        initialize();
        return g_log_path;
    } catch (...) {
        return {};
    }
}

namespace {
struct EarlyStartupMarker {
    EarlyStartupMarker() noexcept
    {
        initialize();
        log("CRT/static initialization reached App.xaml.cpp");
    }
};

EarlyStartupMarker g_early_startup_marker;
} // namespace
} // namespace ps2df::winui::diag

namespace winrt::PS2DriveForge::WinUI::implementation
{
namespace
{
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

void configure_main_window_shell(Window const& window)
{
    try {
        auto root = window.Content().try_as<FrameworkElement>();
        if (!root) {
            ps2df::winui::diag::log("shell layout fix skipped: Window.Content is not a FrameworkElement");
            return;
        }

        // RC6 used the complete visual title-bar Grid as the drag region. That
        // turns its child buttons into non-client input and can overlap the
        // system caption buttons. Keep the visuals, but make only the flexible
        // centre column draggable so toolbar/caption controls remain clickable.
        if (auto title_bar_root = root.FindName(L"TitleBarDragRegion").try_as<Grid>()) {
            Border drag_surface;
            Grid::SetColumn(drag_surface, 1);
            drag_surface.Margin(Thickness{12, 0, 12, 0});
            drag_surface.HorizontalAlignment(HorizontalAlignment::Stretch);
            drag_surface.VerticalAlignment(VerticalAlignment::Stretch);
            drag_surface.Background(Microsoft::UI::Xaml::Media::SolidColorBrush(
                Windows::UI::Colors::Transparent()));
            title_bar_root.Children().InsertAt(0, drag_surface);
            window.SetTitleBar(drag_surface);
            ps2df::winui::diag::log("titlebar drag surface narrowed to centre column");
        }

        // The access InfoBar is intentionally a sibling of all page ScrollViewers.
        // In RC6 that meant the later ScrollViewer could win hit testing and the
        // page content visually occupied the same vertical area. Keep the XAML
        // simple, but reserve the actual measured banner height for every page.
        auto access_bar = root.FindName(L"AccessInfoBar").try_as<InfoBar>();
        std::array<FrameworkElement, 5> pages{
            root.FindName(L"HddPage").try_as<FrameworkElement>(),
            root.FindName(L"FilesPage").try_as<FrameworkElement>(),
            root.FindName(L"MountPage").try_as<FrameworkElement>(),
            root.FindName(L"PerformancePage").try_as<FrameworkElement>(),
            root.FindName(L"SettingsPage").try_as<FrameworkElement>(),
        };
        if (access_bar) {
            Canvas::SetZIndex(access_bar, 100);
            access_bar.SizeChanged([pages](IInspectable const& sender, SizeChangedEventArgs const& args) {
                const auto bar = sender.try_as<InfoBar>();
                const double top = bar && bar.IsOpen() ? args.NewSize().Height + 12.0 : 0.0;
                for (auto const& page : pages) {
                    if (page) page.Margin(Thickness{0, top, 0, 0});
                }
            });
            access_bar.Closed([pages](InfoBar const&, InfoBarClosedEventArgs const&) {
                for (auto const& page : pages) {
                    if (page) page.Margin(Thickness{0, 0, 0, 0});
                }
            });
            ps2df::winui::diag::log("access InfoBar separated from page hit-test/layout area");
        }

        // AdaptiveTrigger uses total window width, which is the wrong metric once
        // NavigationView consumes 48/190 DIP. RC6 therefore changed layout at
        // awkward intermediate widths. Disable those two XAML trigger groups and
        // key the layout off HddContent's real post-navigation width instead.
        auto hdd_content = root.FindName(L"HddContent").try_as<FrameworkElement>();
        auto workspace = root.FindName(L"HddWorkspace").try_as<Grid>();
        auto telemetry_column = root.FindName(L"TelemetryColumn").try_as<ColumnDefinition>();
        auto telemetry_rail = root.FindName(L"TelemetryRail").try_as<StackPanel>();
        auto header_actions = root.FindName(L"HddHeaderActions").try_as<StackPanel>();
        auto header_grid = header_actions ? header_actions.Parent().try_as<Grid>() : nullptr;

        if (workspace) VisualStateManager::GetVisualStateGroups(workspace).Clear();
        if (header_grid) VisualStateManager::GetVisualStateGroups(header_grid).Clear();

        if (hdd_content && workspace && telemetry_column && telemetry_rail && header_actions) {
            auto apply = [workspace, telemetry_column, telemetry_rail, header_actions](double width) {
                const bool header_wide = width >= 900.0;
                Grid::SetRow(header_actions, header_wide ? 0 : 1);
                Grid::SetColumn(header_actions, header_wide ? 1 : 0);
                header_actions.Margin(header_wide ? Thickness{0, 0, 0, 0}
                                                   : Thickness{0, 10, 0, 0});

                const bool telemetry_wide = width >= 1180.0;
                telemetry_column.Width(GridLengthHelper::FromPixels(telemetry_wide ? 300.0 : 0.0));
                Grid::SetRow(telemetry_rail, telemetry_wide ? 0 : 1);
                Grid::SetColumn(telemetry_rail, telemetry_wide ? 1 : 0);
                telemetry_rail.Orientation(Orientation::Vertical);
            };

            hdd_content.SizeChanged([apply](IInspectable const&, SizeChangedEventArgs const& args) {
                apply(args.NewSize().Width);
            });
            apply(hdd_content.ActualWidth());
            ps2df::winui::diag::log("HDD responsive layout now uses actual content width");
        }

        // Small page-level button rows should stack instead of clipping when the
        // content column becomes narrow. These controls are deliberately found
        // by name so the behaviour remains frontend-only and does not touch core.
        auto root_grid = root.FindName(L"RootGrid").try_as<FrameworkElement>();
        auto diagnostics_row = root.FindName(L"RestartAdminButton").try_as<Button>();
        auto mount_row = root.FindName(L"MountPageMountButton").try_as<Button>();
        auto source_row = root.FindName(L"OpenDriveButton").try_as<Button>();
        auto diagnostics_panel = diagnostics_row ? diagnostics_row.Parent().try_as<StackPanel>() : nullptr;
        auto mount_panel = mount_row ? mount_row.Parent().try_as<StackPanel>() : nullptr;
        auto source_panel = source_row ? source_row.Parent().try_as<StackPanel>() : nullptr;
        if (root_grid) {
            root_grid.SizeChanged([diagnostics_panel, mount_panel, source_panel](
                IInspectable const&, SizeChangedEventArgs const& args) {
                const bool compact = args.NewSize().Width < 900.0;
                const auto orientation = compact ? Orientation::Vertical : Orientation::Horizontal;
                if (diagnostics_panel) diagnostics_panel.Orientation(orientation);
                if (mount_panel) mount_panel.Orientation(orientation);
                if (source_panel) source_panel.Orientation(orientation);
            });
        }
    } catch (winrt::hresult_error const& error) {
        const auto message = error.message();
        ps2df::winui::diag::log_hresult("configure_main_window_shell failed", error.code().value,
                                       std::wstring_view(message.c_str(), message.size()));
    } catch (...) {
        ps2df::winui::diag::log("configure_main_window_shell failed with unknown exception");
    }
}
} // namespace

App::App()
{
    ps2df::winui::diag::initialize();
    ps2df::winui::diag::log("App::App entered");

    try {
        ps2df::winui::diag::log("App::InitializeComponent begin");
        InitializeComponent();
        ps2df::winui::diag::log("App::InitializeComponent complete");
    } catch (winrt::hresult_error const& error) {
        const auto message = error.message();
        ps2df::winui::diag::log_hresult(
            "App::InitializeComponent failed",
            error.code().value,
            std::wstring_view(message.c_str(), message.size()));
        throw;
    } catch (std::exception const& error) {
        ps2df::winui::diag::log(std::string("App::InitializeComponent std::exception: ") + error.what());
        throw;
    } catch (...) {
        ps2df::winui::diag::log("App::InitializeComponent failed with unknown exception");
        throw;
    }

    UnhandledException([](
        winrt::Windows::Foundation::IInspectable const&,
        Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& args) {
        try {
            ps2df::winui::diag::log(
                std::string("XAML Application::UnhandledException: ") + winrt::to_string(args.Message()));
        } catch (...) {
            ps2df::winui::diag::log("XAML Application::UnhandledException fired; message conversion failed");
        }
    });
    ps2df::winui::diag::log("App::App complete; XAML unhandled-exception hook installed");
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
{
    ps2df::winui::diag::log("App::OnLaunched entered");
    try {
        ps2df::winui::diag::log("App::OnLaunched creating MainWindow");
        window_ = winrt::make<MainWindow>();
        ps2df::winui::diag::log("App::OnLaunched MainWindow created");
        configure_main_window_shell(window_);
        window_.Activate();
        ps2df::winui::diag::log("App::OnLaunched MainWindow activated");
        ps2df::winui::diag::signal_launcher_ready();
    } catch (winrt::hresult_error const& error) {
        const auto message = error.message();
        ps2df::winui::diag::log_hresult(
            "App::OnLaunched failed",
            error.code().value,
            std::wstring_view(message.c_str(), message.size()));
        throw;
    } catch (std::exception const& error) {
        ps2df::winui::diag::log(std::string("App::OnLaunched std::exception: ") + error.what());
        throw;
    } catch (...) {
        ps2df::winui::diag::log("App::OnLaunched failed with unknown exception");
        throw;
    }
}
}