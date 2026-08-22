# WinUI startup diagnostics

The Emilia WinUI frontend writes a best-effort startup/crash trace to:

```text
%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log
```

The file is append-only across launches. Every line contains a timestamp, process ID and thread ID. The logger is initialized during CRT/static initialization so it can separate loader/startup failures from `App`, XAML, `MainWindow`, custom-titlebar and activation failures.

## Normal diagnostic path

For a WinUI startup failure:

1. start the normal root `PS2-DriveForge.exe` launcher;
2. if the launcher offers recovery, open the startup log or start the Win32 fallback;
3. inspect `%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log`;
4. preserve the lines for the most recent PID;
5. report the last successful stage and any `HRESULT`, `XAML Application::UnhandledException`, `std::terminate`, or native-exception line.

The messages are also mirrored through `OutputDebugString`, so DebugView or an attached debugger can capture them live.

The logger flushes every entry immediately. Logging failures are intentionally ignored so the diagnostic system cannot become a new startup dependency.

## No log at all

If WinUI exits without creating a log, the failure occurred before DriveForge's own CRT/static initializer. Investigate Windows loader/runtime state, Event Viewer/WER and PE imports before treating the problem as an XAML exception.

The root launcher has its own loader-level protection: release staging executes:

```powershell
.\PS2-DriveForge.exe --self-test
```

before ZIP/setup creation. This catches binaries that compiled successfully but cannot be loaded on Windows because of an unavailable import.

## RC findings preserved as regression knowledge

### Self-contained payload originally incomplete

An early Emilia package contained the WinUI EXE and generated metadata but not the actual Windows App SDK runtime. The release verifier now requires real runtime files such as `Microsoft.UI.Xaml.dll` and Windows App Runtime components rather than treating the existence of the EXE as proof of a self-contained deployment.

### Missing `AccentFillColorDefaultBrush`

The first real-machine trace after runtime staging was repaired reached:

```text
MainWindow::InitializeComponent()
```

and failed with `HRESULT 0x802B000A` because `AccentFillColorDefaultBrush` was not available in the runtime resource dictionaries on the validation machine.

DriveForge now owns semantic accent/caution/success brushes in `App.xaml` instead of relying on optional theme keys for application-specific styling.

### Missing `TabViewButtonBackground`

The next real-machine trace reached the same XAML construction phase and failed on `TabViewButtonBackground`. That key is part of WinUI control resources rather than a DriveForge-specific color alias. The underlying issue was that the unpackaged application had not merged the WinUI control resource dictionary.

`App.xaml` now merges:

```text
Microsoft.UI.Xaml.Controls.XamlControlsResources
```

so `NavigationView`, `TabView`-derived templates and other WinUI controls resolve their normal Fluent resources from the intended source. This is the correct fix; adding one compatibility alias per missing internal key would merely hide the missing dictionary.

### Root launcher `COMCTL32` ordinal failure

RC4's root launcher used `TaskDialogIndirect`. The produced PE statically imported a common-controls ordinal that was not available through the DLL resolved on the validation machine, causing Windows to abort the process before `wWinMain()`.

Symptoms included:

- an `ordinal 345 could not be located` loader dialog;
- no launcher recovery UI;
- `PS2-DriveForge.exe --legacy` also failing, because argument parsing never ran.

The launcher no longer depends on `TaskDialogIndirect`/COMCTL32 for recovery. It uses stable USER32 primitives and the packaging pipeline runs the staged `--self-test`. PE inspection of the corrected launcher shows no COMCTL32 import.

## What to attach to a bug report

For a startup crash, include:

- exact DriveForge version/RC and SHA if known;
- Windows version/build;
- whether root launcher, direct WinUI, or `--legacy` was used;
- `winui-startup.log` for the failing PID;
- screenshot/text of any Windows loader dialog;
- whether Win32 fallback starts successfully.

Do not include unrelated personal files or disk contents. The startup log is designed to contain initialization/diagnostic information rather than PS2 filesystem payload data.
