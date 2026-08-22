# Windows frontends and packaging

Emilia intentionally ships **two user interfaces over one storage stack**.

## Entry point

The user-facing executable is:

```text
PS2-DriveForge.exe
```

It is a small native launcher, not the storage application itself. Its responsibilities are deliberately narrow:

1. locate `app\winui\PS2-DriveForge-WinUI.exe`;
2. create a named startup-ready event and pass its name to WinUI;
3. start WinUI with `app\winui` as its working directory;
4. wait for either the post-activation ready signal, early process exit or a bounded timeout;
5. offer the supported Win32 fallback/log path after an early WinUI failure.

The launcher does not parse APA/PFS, open raw disks or mount Dokany filesystems.

## Why there is a launcher

A direct WinUI EXE is not enough for a user-friendly storage tool. Real-machine RC testing has already observed failures that occurred:

- before WinUI created a window;
- during XAML resource construction;
- before the launcher's own `wWinMain()` because of a loader import.

The bootstrap layer provides one stable user entry point, explicit recovery and a place to enforce startup diagnostics without duplicating the DriveForge backend.

## Startup handshake

WinUI receives the event name through:

```text
PS2DF_WINUI_READY_EVENT
```

It signals readiness only after the main Window has been created and activated. A process that exists for a few seconds is not automatically considered healthy.

The packaging pipeline also runs:

```text
PS2-DriveForge.exe --self-test
```

against the **staged** launcher. This catches loader/import failures and missing expected payload paths before Setup/Portable artifacts are created.

## Supported Win32 fallback

The fallback is:

```text
legacy\PS2-DriveForge-Win32.exe
```

It is not deprecated in 0.5. It remains supported because it has broader real-hardware validation and gives users a functional path when WinUI/XAML/runtime behavior differs on a particular Windows installation.

Direct launch:

```powershell
.\PS2-DriveForge.exe --legacy
```

Both GUI frontends are expected to consume the shared native host/core behavior. A storage correctness fix belongs in the shared layers, not in one UI.

## WinUI layout

The portable package hides self-contained runtime detail below:

```text
app\winui\
```

instead of placing hundreds of Windows App SDK files next to the root launcher. This is an implementation directory; normal users should not need to open it.

The **private RC5** payload is built with Windows App SDK 2.3.1 and resolves WinUI 2.3.0. Microsoft has identified that WinUI package as affected by an incorrect Engineering Preview license and instructs affected publishers to update to Windows App SDK **2.4.0**, which resolves WinUI 2.3.6, before publishing. The next public candidate must therefore bump the dependency and repeat WinUI/package/real-machine startup validation. See [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

Whether the final public build remains self-contained or moves to the official Windows App Runtime prerequisite model is a deployment/UX decision made after that dependency bump; either way the runtime details remain below the user-facing launcher rather than cluttering the root directory.

## Startup log

WinUI diagnostics:

```text
%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log
```

See [`winui-diagnostics.md`](winui-diagnostics.md) for stage markers and preserved RC regressions.

## Theme behavior

Win32 supports System/Light/Dark with High Contrast passthrough. Controls use explicit palette colors where native common-controls dark behavior is unreliable. The bottom status bar receives dedicated dark-mode painting so text contrast does not depend on a native default that may remain black.

WinUI uses WinUI/Fluent resources plus DriveForge-owned semantic brushes. The application explicitly merges `Microsoft.UI.Xaml.Controls.XamlControlsResources`; control-internal theme keys must come from the WinUI resource dictionary rather than being recreated one at a time.

## Mount behavior

Frontend mount requests share the same Dokany controller/policy. Automatic drive-letter selection scans **C: through Z:** and chooses the first free letter. A: and B: are excluded.

## User package

```text
PS2-DriveForge.exe
README.md
CHANGELOG.md
LICENSE
CREDITS.md
THIRD_PARTY_NOTICES.md
app\winui\...
legacy\PS2-DriveForge-Win32.exe
tools\...
docs\...
```

The recommended public download is the installer; Portable is an alternative for users who deliberately want a self-contained directory. Regression executables stay in canonical CI staging and are not part of the normal user package.
