# Darkness 0.4 development plan

Darkness adds a read-only Windows filesystem provider on top of the hardware-validated Chisato stack. It also tightens the Windows host experience around that provider; host-UI behavior remains isolated from APA/PFS format code.

## Definition of done

A supported PS2 HDD image or physical `PhysicalDriveN` can be mounted through Dokany and browsed in Windows Explorer. PFS files copied through Explorer must be byte-identical to DriveForge's direct reader/export path. Every source-device operation remains read-only.

The native management GUI should also be comfortable for long Windows 11 sessions: System/Light/Dark modes are supported and High Contrast is never overridden.

## Namespace

Initial 0.4 namespace:

```text
<mount>\
  Partitions\
    <PFS partition>\
      <PFS tree>
```

`Games` and synthetic HDL ISO views remain 0.7 Guts work. MBR/recovery synthetic files remain later work.

## Implementation order

1. **Done:** portable `ReadOnlyMountView` for lookup/list/read path mapping;
2. **Done:** thread-safe `DriveSession::stat` and `DriveSession::read_file` operations;
3. **Done:** deterministic Windows-safe aliases for PFS/APA path components;
4. **Done / CI-built:** Dokany 2.3.1 adapter (`ZwCreateFile`, `ReadFile`, `GetFileInformation`, `FindFiles`, volume/free-space callbacks);
5. **Done / CI-built:** hard rejection of mutating callbacks and `DOKAN_OPTION_WRITE_PROTECT`;
6. **Done / CI-built:** CLI mount frontend for image and `PhysicalDriveN` sources;
7. **Done:** Windows CI installs the pinned Dokany 2.3.1 development SDK/runtime, verifies the MSI SHA-256, compiles and packages the mount frontend;
8. **In progress:** generated-image mounted-read tests where CI permits, otherwise callback-independent mount-view tests plus manual Windows mount smoke test;
9. **Pending:** real-HDD Explorer validation and SHA-256 comparison.

The portable mount-view tests already cover root/partition enumeration, Windows-style case-insensitive lookup, random-offset reads and EOF clamping under the normal sanitizer/MSVC matrix.

## Safety invariants

- no `BlockDevice::write()` is introduced;
- `PhysicalDrive` continues to request `GENERIC_READ` only;
- Dokany is an adapter above `ReadOnlyMountView`/`DriveSession`, never a parser layer;
- create/write/delete/rename/truncate/attribute mutation operations return write-protected/access-denied status;
- the mount requests `DOKAN_OPTION_WRITE_PROTECT` in addition to callback-level rejection;
- source-device handles never become writable as a side effect of mounting.

## Windows GUI theme policy

Darkness adds persistent `View -> Theme -> System / Light / Dark` selection to the native Win32 GUI.

- **System** follows the Windows application colour preference (`AppsUseLightTheme`).
- **Light** and **Dark** persist under the current user's DriveForge registry settings.
- Windows High Contrast always takes precedence over a DriveForge Light/Dark override.
- The client background, TreeView, ListView/header and status bar receive explicit palette colours.
- The Windows 11 non-client frame uses the documented DWM `DWMWA_USE_IMMERSIVE_DARK_MODE` path.
- Native dark menu/common-control behavior is enabled through dynamically resolved UxTheme helpers as a best-effort enhancement only. These are not a parser/runtime dependency: if Windows removes those optional exports, DriveForge falls back to standard system menu rendering rather than failing to launch.
- The window class does not own a fixed white background brush; `WM_ERASEBKGND` uses the active palette to avoid a bright flash during dark-mode startup and resizing, which is particularly noticeable on HDR/OLED displays.

This theme policy is deliberately a host-UI concern. Future non-Windows frontends should map their own **System** preference to the desktop environment rather than importing Win32 theme behavior into shared core code.

## Performance boundary

Darkness targets filesystem correctness and stable Explorer behavior. Major caching, read-ahead, request coalescing and overlapped physical I/O remain 0.5 Emilia. Instrumentation should remain enabled so Darkness workloads produce the baseline Emilia will optimize.

## Hardware validation target

On the current real test HDD:

1. mount the PS2 HDD read-only;
2. browse `Partitions\+OPL` in Explorer;
3. browse `Partitions\__common\OPL`;
4. copy `conf_hdd.cfg` through Explorer;
5. verify SHA-256 `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`;
6. recursively copy `+OPL`;
7. verify file creation, rename, deletion and writes are rejected;
8. unmount cleanly;
9. record backing-I/O statistics for the mounted workload;
10. visually verify GUI System/Dark/Light switching on Windows 11, including title bar, panes, header/status area and menu fallback behavior.

Generated-image tests continue to cover SEGI and APA main/sub-partition crossings that are not available as real PFS content on the current HDD.
