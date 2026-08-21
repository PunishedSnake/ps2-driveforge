# Darkness 0.4 development plan

Darkness adds a read-only Windows filesystem provider on top of the hardware-validated Chisato stack. It also tightens the Windows host experience around that provider; host-UI behavior remains isolated from APA/PFS format code.

## Definition of done

A supported PS2 HDD image or physical Windows disk can be discovered, opened and mounted read-only through the native DriveForge GUI, then browsed in Windows Explorer. PFS files copied through Explorer must be byte-identical to DriveForge's direct reader/export path. Every source-device operation remains read-only.

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

## Implementation status

1. **Done:** portable `ReadOnlyMountView` for lookup/list/read path mapping;
2. **Done:** thread-safe `DriveSession::stat` and `DriveSession::read_file` operations;
3. **Done:** deterministic Windows-safe aliases for PFS/APA path components;
4. **Done / hardware validated:** Dokany 2.3.1 read-only adapter (`ZwCreateFile`, `ReadFile`, metadata, directory and volume callbacks);
5. **Done / hardware validated:** layered mutation rejection with `DOKAN_OPTION_WRITE_PROTECT`, callback-level rejection, no `BlockDevice::write()`, and `GENERIC_READ` physical-drive handles;
6. **Done:** standalone mount CLI retained as a diagnostic/script frontend;
7. **Done:** shared `DokanyMountController` owns the callback implementation and is used directly by both GUI and CLI; the GUI does not launch a helper process;
8. **Done:** Windows disk discovery now enumerates actual `GUID_DEVINTERFACE_DISK` devices through SetupAPI and maps them to `PhysicalDriveN` using `IOCTL_STORAGE_GET_DEVICE_NUMBER` before running the DriveForge APA probe;
9. **Done:** the fixed visible `PhysicalDrive0..31` menu has been removed; the GUI presents only detected PS2 APA candidates, including friendly device name, size, APA version and main-partition count;
10. **Done:** discovery starts automatically with the GUI and can be repeated with `Rescan PS2 HDDs`; a single PS2 APA candidate is opened automatically;
11. **Done:** normal-user GUI startup uses a controlled UAC `runas` relaunch with an internal loop-prevention marker; cancelling UAC leaves a limited image-capable process instead of terminating the application;
12. **Done / CI-built:** GUI commands for `Mount read-only`, `Open mounted volume in Explorer`, and `Unmount`, with automatic selection of a free drive letter (preferring `P:`);
13. **Done:** Windows CI installs the pinned Dokany 2.3.1 development SDK/runtime, verifies the MSI SHA-256, compiles and packages the GUI/CLI/controller stack;
14. **Pending hardware smoke test:** validate the new integrated GUI discovery/elevation/mount workflow on the real PS2 HDD before merging Darkness.

## Windows disk discovery

DriveForge no longer guesses which `PhysicalDriveN` paths might exist. Discovery follows Windows' device model:

```text
SetupDiGetClassDevs(GUID_DEVINTERFACE_DISK)
        -> SetupDiEnumDeviceInterfaces
        -> disk device-interface path
        -> IOCTL_STORAGE_GET_DEVICE_NUMBER
        -> actual PhysicalDriveN
        -> GENERIC_READ PhysicalDrive
        -> DriveForge APA probe
```

The interface handle used to obtain `STORAGE_DEVICE_NUMBER` requests no data access. The resulting raw disk is then reopened through the normal DriveForge `PhysicalDrive` backend, which remains `GENERIC_READ` only. APA identity is decided by the parser, not by model name, capacity or Windows partition-table heuristics.

This removes the old arbitrary 0..31 scan limit from the GUI and avoids presenting nonexistent/non-PS2 disks as user choices.

## Elevation policy

Raw-disk access commonly requires an elevated token on Windows. DriveForge does not use a `requireAdministrator` manifest because disk-image browsing does not intrinsically require elevation.

At normal GUI startup:

```text
normal user
   -> ShellExecuteExW("runas", --elevated-relaunch)
      -> elevated DriveForge
      -> automatic disk-interface discovery
```

The `--elevated-relaunch` marker prevents an accidental UAC relaunch loop. If the user cancels elevation, DriveForge continues in a limited mode so disk images remain usable; `Restart as Administrator` is available in the File menu.

## Shared mount controller

`PS2-DriveForge-Mount.exe` remains useful for scripts, diagnostics and callback logging, but it no longer owns a separate filesystem implementation.

```text
Native GUI                mount CLI
    |                         |
    +--- DokanyMountController+
                 |
          ReadOnlyMountView
                 |
            DriveSession
```

`DokanyMountController` owns source lifetime, `DokanMain`, callbacks, mount/unmount state and a worker thread. The GUI can therefore mount/unmount directly without spawning a console helper while the CLI remains a thin frontend over the same implementation.

The GUI suggests `P:` when free, otherwise another unused drive letter. Opening a mounted volume uses the normal Windows shell/Explorer path.

## Safety invariants

- no `BlockDevice::write()` is introduced;
- `PhysicalDrive` continues to request `GENERIC_READ` only;
- SetupAPI enumeration does not make Windows device identity a filesystem-format decision;
- Dokany is an adapter above `ReadOnlyMountView`/`DriveSession`, never a parser layer;
- create/write/delete/rename/truncate/attribute mutation operations return write-protected/access-denied status;
- the mount requests `DOKAN_OPTION_WRITE_PROTECT` in addition to callback-level rejection;
- source-device handles never become writable as a side effect of discovery or mounting;
- GUI and CLI share one callback/controller implementation.

## ZwCreateFile disposition contract

Dokany's `ZwCreateFile` callback receives the NT kernel `FILE_*` create-disposition values, not the Win32 `CreateFileW` constants.

This distinction is deliberately covered by a portable regression test because several numeric values overlap while having different meanings. The first real Darkness mount exposed exactly this trap: `FILE_OPEN == 1` was accidentally interpreted as Win32 `CREATE_NEW == 1`, so opening the existing mount root returned `STATUS_OBJECT_NAME_COLLISION` and Explorer displayed **"The file exists."**

The adapter models the NT values explicitly:

```text
0 FILE_SUPERSEDE
1 FILE_OPEN
2 FILE_CREATE
3 FILE_OPEN_IF
4 FILE_OVERWRITE
5 FILE_OVERWRITE_IF
```

Existing objects opened with `FILE_OPEN`/`FILE_OPEN_IF` succeed when no write access is requested. Create/overwrite/supersede operations remain blocked by the read-only policy. `ps2-driveforge-dokany-open-policy-tests` runs under both MSVC and Linux sanitizer CI.

The standalone diagnostic frontend supports:

```powershell
PS2-DriveForge-Mount.exe --physical 3 --mount P: --debug
```

which logs create/open, directory, metadata, read and volume/free-space callbacks without changing source-device access.

## Windows GUI theme policy

Darkness adds persistent `View -> Theme -> System / Light / Dark` selection to the native Win32 GUI.

- **System** follows the Windows application colour preference (`AppsUseLightTheme`).
- **Light** and **Dark** persist under the current user's DriveForge registry settings.
- Windows High Contrast always takes precedence over a DriveForge Light/Dark override.
- The client background, TreeView, ListView/header and status bar receive explicit palette colours.
- The Windows 11 non-client frame uses the documented DWM `DWMWA_USE_IMMERSIVE_DARK_MODE` path.
- Native dark menu/common-control behavior is enabled through dynamically resolved UxTheme helpers as a best-effort enhancement only.
- The window class does not own a fixed white background brush; `WM_ERASEBKGND` uses the active palette to avoid bright flashes during startup/resizing on HDR/OLED displays.

Theme policy remains a host-UI concern; shared APA/PFS/session/mount code does not depend on it.

## Performance boundary

Darkness targets filesystem correctness and stable Explorer behavior. Major caching, read-ahead, request coalescing and overlapped physical I/O remain 0.5 Emilia. Explorer's real callback workload has already shown repeated metadata requests, giving Emilia a concrete optimization target.

## Hardware validation progress

The original CLI-driven Darkness mount has been validated on the real 149.05 GiB APA v2 HDD:

- `PhysicalDrive3` mounted read-only as `P:`;
- Explorer opened `P:\` and reported the PS2PFS volume;
- `P:\Partitions\+OPL` and `P:\Partitions\__common\OPL` enumerated correctly;
- `conf_hdd.cfg` copied through Explorer was 20 bytes and matched SHA-256 `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`;
- creation of a write-test file was rejected;
- unmount completed cleanly;
- System/Dark/Light GUI switching was visually validated on Windows 11.

The remaining Darkness hardware gate is specifically the **new integrated GUI workflow**:

1. launch the GUI as a normal user and confirm automatic UAC relaunch;
2. confirm startup SetupAPI discovery identifies the PS2 HDD without a fixed PhysicalDrive list;
3. confirm a single PS2 candidate opens automatically and displays the expected 149.05 GiB / APA v2 / 43-main-partition state;
4. use `File -> Mount read-only` and confirm a free drive letter is selected automatically;
5. use `Open mounted volume in Explorer`, browse `Partitions\+OPL` and `Partitions\__common\OPL`;
6. optionally copy/hash `conf_hdd.cfg` again as a regression;
7. use the GUI `Unmount` command and confirm the drive letter disappears cleanly;
8. rescan and source switching must not expose a writable raw-disk path.

Generated-image tests continue to cover SEGI and APA main/sub-partition crossings that are not available as real PFS content on the current HDD.
