#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "StartupLog.hpp"

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
