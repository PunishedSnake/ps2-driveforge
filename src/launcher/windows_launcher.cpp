#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace {
constexpr wchar_t kReadyEventEnv[] = L"PS2DF_WINUI_READY_EVENT";
constexpr DWORD kStartupHandshakeTimeoutMs = 15000;

std::filesystem::path executable_directory()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
}

std::filesystem::path startup_log_path()
{
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return executable_directory() / L"winui-startup.log";
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)) /
           L"PS2 DriveForge" / L"Logs" / L"winui-startup.log";
}

bool launch_process(const std::filesystem::path& executable, const std::filesystem::path& working_directory,
                    PROCESS_INFORMATION* process_information = nullptr)
{
    if (!std::filesystem::exists(executable)) {
        return false;
    }

    std::wstring command_line = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    const BOOL created = CreateProcessW(
        executable.c_str(),
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        working_directory.c_str(),
        &startup,
        &process);
    if (!created) {
        return false;
    }

    if (process_information) {
        *process_information = process;
    } else {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    return true;
}

bool launch_legacy(const std::filesystem::path& root)
{
    const auto legacy = root / L"legacy" / L"PS2-DriveForge-Win32.exe";
    return launch_process(legacy, legacy.parent_path());
}

void open_log_or_folder()
{
    const auto log = startup_log_path();
    const auto target = std::filesystem::exists(log) ? log : log.parent_path();
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

int fallback_dialog(const std::filesystem::path& root, std::wstring_view detail)
{
    constexpr int kOpenLegacy = 1001;
    constexpr int kOpenLog = 1002;
    constexpr int kExit = 1003;

    const TASKDIALOG_BUTTON buttons[] = {
        {kOpenLegacy, L"Open Win32 fallback\nStart the proven legacy interface using the same DriveForge backend."},
        {kOpenLog, L"Open startup log\nShow the WinUI diagnostic log for troubleshooting."},
        {kExit, L"Exit"},
    };

    std::wstring content = L"The modern WinUI frontend could not finish starting.";
    if (!detail.empty()) {
        content += L"\n\n";
        content += detail;
    }
    content += L"\n\nYour PS2 HDD has not been modified by this startup failure.";

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"PS2 DriveForge — Emilia";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = L"DriveForge WinUI could not start";
    config.pszContent = content.c_str();
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = kOpenLegacy;

    int pressed = kExit;
    if (SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) {
        if (pressed == kOpenLegacy) {
            if (!launch_legacy(root)) {
                MessageBoxW(nullptr,
                            L"The legacy Win32 fallback could not be started.\n\nExpected: legacy\\PS2-DriveForge-Win32.exe",
                            L"PS2 DriveForge",
                            MB_OK | MB_ICONERROR);
                return 2;
            }
            return 0;
        }
        if (pressed == kOpenLog) {
            open_log_or_folder();
            return 1;
        }
    }
    return 1;
}

bool has_argument(std::wstring_view expected)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return false;
    }

    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], expected.data()) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const auto root = executable_directory();

    if (has_argument(L"--legacy")) {
        if (!launch_legacy(root)) {
            MessageBoxW(nullptr,
                        L"The legacy Win32 fallback could not be started.\n\nExpected: legacy\\PS2-DriveForge-Win32.exe",
                        L"PS2 DriveForge",
                        MB_OK | MB_ICONERROR);
            return 2;
        }
        return 0;
    }

    const auto winui = root / L"app" / L"winui" / L"PS2-DriveForge-WinUI.exe";
    if (!std::filesystem::exists(winui)) {
        return fallback_dialog(root, L"The WinUI executable is missing from app\\winui.");
    }

    const std::wstring event_name =
        L"Local\\PS2DriveForge.WinUI.Ready." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    HANDLE ready_event = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    if (!ready_event) {
        return fallback_dialog(root, L"DriveForge could not create the WinUI startup handshake.");
    }

    SetEnvironmentVariableW(kReadyEventEnv, event_name.c_str());
    PROCESS_INFORMATION process{};
    const bool launched = launch_process(winui, winui.parent_path(), &process);
    SetEnvironmentVariableW(kReadyEventEnv, nullptr);

    if (!launched) {
        CloseHandle(ready_event);
        return fallback_dialog(root, L"Windows could not create the WinUI process.");
    }

    CloseHandle(process.hThread);
    HANDLE handles[] = {process.hProcess, ready_event};
    const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, kStartupHandshakeTimeoutMs);

    if (wait == WAIT_OBJECT_0 + 1) {
        CloseHandle(process.hProcess);
        CloseHandle(ready_event);
        return 0;
    }

    if (wait == WAIT_OBJECT_0) {
        DWORD exit_code = 0;
        GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hProcess);
        CloseHandle(ready_event);
        return fallback_dialog(root, L"The WinUI process exited before its main window became ready (exit code " +
                                         std::to_wstring(exit_code) + L").");
    }

    CloseHandle(process.hProcess);
    CloseHandle(ready_event);

    // Do not kill a slow-but-running frontend. The launcher exists only to catch
    // deterministic startup failures; after the handshake timeout the WinUI
    // process owns its own lifetime and diagnostics.
    if (wait == WAIT_TIMEOUT) {
        return 0;
    }

    return fallback_dialog(root, L"The WinUI startup handshake failed unexpectedly.");
}
