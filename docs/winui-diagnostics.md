# WinUI startup diagnostics

The Emilia WinUI frontend writes a best-effort startup/crash trace to:

```text
%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log
```

The file is append-only across launches. Every line contains a local timestamp, process ID and thread ID. The logger is initialized during CRT/static initialization so it can distinguish failures before `App`, during XAML application initialization, during `MainWindow` XAML construction, during custom title-bar setup, and during activation.

For a startup failure:

1. launch `WinUI\PS2-DriveForge-WinUI.exe` once;
2. open `%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log`;
3. preserve the lines for the most recent PID;
4. report the last successful stage plus any `HRESULT`, `XAML Application::UnhandledException`, `std::terminate`, or `native unhandled exception` line.

The same messages are mirrored through `OutputDebugString`, so Sysinternals DebugView or an attached debugger can capture them live.

The logger flushes every entry immediately. Logging failures are intentionally ignored so diagnostics cannot become a new reason for the frontend to fail.

If the process exits without creating any log file at all, the failure happened before DriveForge's own CRT/static initialization (for example, loader/runtime startup) and should be investigated with Windows Event Viewer/WER or a debugger rather than as an `App`/XAML exception.
