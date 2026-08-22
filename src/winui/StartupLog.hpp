#pragma once

#include <string>
#include <string_view>

namespace ps2df::winui::diag {

// Initializes the WinUI startup logger and native crash hooks. Safe to call more
// than once and intentionally noexcept so diagnostics cannot become a new startup
// failure mode.
void initialize() noexcept;

// Appends one UTF-8 line to the startup log and mirrors it to OutputDebugString.
void log(std::string_view message) noexcept;

// Formats a WinRT/HRESULT failure without requiring WinRT types in this header.
void log_hresult(std::string_view context, long code, std::wstring_view message) noexcept;

// Returns the current log path. The normal location is:
// %LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log
std::wstring log_path() noexcept;

} // namespace ps2df::winui::diag
