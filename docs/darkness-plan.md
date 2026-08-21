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

## ZwCreateFile disposition contract

Dokany's `ZwCreateFile` callback receives the NT kernel `FILE_*` create-disposition values, not the Win32 `CreateFileW` constants.

This distinction is deliberately covered by a portable regression test because several numeric values overlap while having different meanings. The first real Darkness mount exposed exactly this trap: `FILE_OPEN == 1` was accidentally interpreted as Win32 `CREATE_NEW == 1`, so opening the existing mount root returned `STATUS_OBJECT_NAME_COLLISION` and Explorer displayed **"The file exists."**

The adapter now models the NT values explicitly:

```text
0 FILE_SUPERSEDE
1 FILE_OPEN
2 FILE_CREATE
3 FILE_OPEN_IF
4 FILE_OVERWRITE
5 FILE_OVERWRITE_IF
```

Existing objects opened with `FILE_OPEN`/`FILE_OPEN_IF` succeed when no write access is requested. Create/overwrite/supersede operations remain blocked by the read-only policy. The `ps2-driveforge-dokany-open-policy-tests` regression runs under both MSVC and Linux sanitizer CI, taking the normal test matrix from seven to eight executables.

For real-machine diagnosis the mount frontend also supports:

```powershell
PS2-DriveForge-Mount.exe --physical 3 --mount P: --debug
```

which logs `ZwCreateFile`, directory enumeration, metadata, read and volume/free-space callbacks without changing source-device access.

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

### Real-hardware progress

- The native GUI System/Dark/Light switching has been visually validated on Windows 11 and changes dynamically while the application is running.
- Dokany successfully created the `P:` mount point for `PhysicalDrive3`.
- The first Explorer open failed at the root with "The file exists" because of the NT/Win32 create-disposition mismatch described above.
- The corrected adapter, `--debug` callback logging, and the new disposition-policy regression pass both MSVC and Clang ASan/UBSan CI.
- The corrected mount build now requires the second Explorer smoke test before the remaining browse/copy/write-rejection checks continue.

Generated-image tests continue to cover SEGI and APA main/sub-partition crossings that are not available as real PFS content on the current HDD.
