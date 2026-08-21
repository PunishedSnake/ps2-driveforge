#include "windows_elevation.hpp"

#include <shellapi.h>

#include <array>

namespace ps2driveforge::gui {

bool is_process_elevated() noexcept
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &returned);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

ElevationAttempt relaunch_elevated(bool relaunch_marker_present)
{
    if (is_process_elevated()) {
        return ElevationAttempt::already_elevated;
    }
    if (relaunch_marker_present) {
        return ElevationAttempt::failed;
    }

    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        return ElevationAttempt::failed;
    }

    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_NOCLOSEPROCESS;
    request.lpVerb = L"runas";
    request.lpFile = executable.data();
    request.lpParameters = L"--elevated-relaunch";
    request.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&request)) {
        return GetLastError() == ERROR_CANCELLED ? ElevationAttempt::cancelled
                                                 : ElevationAttempt::failed;
    }
    if (request.hProcess) {
        CloseHandle(request.hProcess);
    }
    return ElevationAttempt::relaunched;
}

} // namespace ps2driveforge::gui
