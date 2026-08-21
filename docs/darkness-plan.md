# Darkness 0.4 development plan

Darkness adds a read-only Windows filesystem provider on top of the hardware-validated Chisato stack and finishes the Windows host workflow needed before the 0.5 performance pass.

## Definition of done

A supported PS2 HDD image or physical Windows disk can be discovered, opened, mounted read-only from the native GUI, browsed in Windows Explorer, and unmounted cleanly. Files copied through Explorer must be byte-identical to DriveForge's direct reader/export path. Every source-device operation remains read-only.

The native Windows GUI also supports persistent System/Light/Dark themes with High Contrast passthrough.

## Namespace

```text
<mount>:\
  Partitions\
    <PFS partition>\
      <PFS tree>
```

`Games`/HDL and synthetic MBR/recovery views remain later milestones.

## Implementation status

1. **Done:** portable `ReadOnlyMountView` for lookup/list/read mapping.
2. **Done:** thread-safe `DriveSession::stat` and random-offset `read_file` operations.
3. **Done:** deterministic Windows-safe aliases without changing original APA/PFS names internally.
4. **Done / hardware validated:** Dokany 2.3.1 read-only callbacks and Explorer volume behavior.
5. **Done / hardware validated:** layered write protection: no `BlockDevice::write()`, `GENERIC_READ` raw handles, `DOKAN_OPTION_WRITE_PROTECT`, and callback-level mutation rejection.
6. **Done:** standalone mount CLI retained for diagnostics/scripts.
7. **Done:** shared `DokanyMountController` is used directly by both GUI and CLI; no helper-process filesystem implementation exists.
8. **Done:** SetupAPI `GUID_DEVINTERFACE_DISK` enumeration maps actual Windows disk interfaces to physical-drive numbers through `IOCTL_STORAGE_GET_DEVICE_NUMBER` before the normal DriveForge APA probe.
9. **Done:** fixed visible `PhysicalDrive0..31` GUI list removed; only detected PS2 APA candidates are shown with friendly name, size, APA version, and main-partition count.
10. **Done:** automatic startup discovery plus `Rescan PS2 HDDs`; a single candidate auto-opens only when no source is already open.
11. **Done:** controlled UAC `runas` relaunch with loop prevention, cancellation fallback to image-capable limited mode, and explicit `Restart as Administrator`.
12. **Done / CI-built:** direct GUI `Mount read-only`, `Open mounted volume in Explorer`, and `Unmount` commands.
13. **Done:** automatic free-drive-letter selection preferring `P:` and avoiding letters below `D:`.
14. **Done:** portable NT `ZwCreateFile` policy regression protects the original root-open bug.
15. **Done:** portable Darkness GUI/mount policy regression protects one-candidate auto-open and deterministic free-letter fallback/exhaustion behavior.
16. **Done:** Windows CI pins/verifies/installs Dokany 2.3.1 SDK/runtime and packages GUI, inspector, mount CLI, docs, and all nine regression tests.
17. **Done:** CLI-driven real-HDD Dokany path validates Explorer browse, known-file copy/hash, write rejection, and clean unmount.
18. **Pending hardware gate:** validate the final integrated GUI elevation/discovery/mount workflow on the real PS2 HDD.

## Windows disk discovery

DriveForge does not infer a fixed range of `PhysicalDriveN` paths. Discovery follows the Windows storage device model:

```text
SetupDiGetClassDevs(GUID_DEVINTERFACE_DISK)
  -> SetupDiEnumDeviceInterfaces
  -> disk interface
  -> IOCTL_STORAGE_GET_DEVICE_NUMBER
  -> actual PhysicalDriveN
  -> GENERIC_READ DriveForge backend
  -> APA parser
```

The device-interface handle used to obtain `STORAGE_DEVICE_NUMBER` requests no data access. The raw disk is reopened through the normal DriveForge read-only backend. PS2 identity comes from APA validation, not model name, capacity, or Windows partition-table heuristics.

## Elevation policy

DriveForge deliberately does not use a global `requireAdministrator` manifest because image browsing does not need elevation.

Normal startup:

```text
normal user
  -> ShellExecuteExW("runas", --elevated-relaunch)
  -> elevated DriveForge
  -> SetupAPI discovery
```

The marker prevents relaunch loops. If UAC is cancelled, the original GUI remains running in limited mode so image files still work. `Restart as Administrator` can recover raw-disk access later.

## Shared mount controller

```text
Native GUI                 mount CLI
    |                         |
    +--- DokanyMountController+
                 |
          ReadOnlyMountView
                 |
            DriveSession
```

The controller owns source lifetime, `DokanMain`, callbacks, mount/unmount state, and its worker thread. The standalone console program is a thin developer frontend over the same implementation used by the GUI.

## Safety invariants

- no source `write()` capability exists;
- raw PS2 HDD access remains `GENERIC_READ`;
- SetupAPI discovery is classification, never target selection for mutation;
- Dokany does not parse APA/PFS;
- create/write/delete/rename/truncate/attribute mutation stays rejected;
- `DOKAN_OPTION_WRITE_PROTECT` provides an additional runtime barrier;
- GUI and CLI share one provider/controller implementation.

## Dokany create-disposition contract

`ZwCreateFile` receives NT kernel `FILE_*` dispositions, not Win32 `CreateFileW` constants. The first physical mount exposed this because `FILE_OPEN == 1` was interpreted as Win32 `CREATE_NEW == 1`, returning a root collision and Explorer's **"The file exists."** message.

The adapter now models:

```text
0 FILE_SUPERSEDE
1 FILE_OPEN
2 FILE_CREATE
3 FILE_OPEN_IF
4 FILE_OVERWRITE
5 FILE_OVERWRITE_IF
```

and a portable regression runs under both MSVC and Linux sanitizer CI.

## Windows theme policy

- `View -> Theme -> System / Light / Dark` persists per user.
- System follows Windows' application theme preference.
- High Contrast overrides DriveForge theme choices.
- Client area, TreeView, ListView/header, status area, and Windows 11 titlebar receive dark-mode handling.
- optional UxTheme helpers are best-effort only; failure falls back instead of blocking startup.
- theme code remains entirely above storage/session/parser layers.

## Performance boundary and Emilia handoff

Darkness targets filesystem correctness and stable Explorer behavior. Caching, read-ahead, request coalescing, and overlapped physical I/O are intentionally 0.5 Emilia work.

The final Darkness workload to preserve before merge is:

```text
cold GUI start
 -> UAC/discovery/auto-open
 -> mount
 -> root
 -> Partitions
 -> +OPL
 -> __common\OPL
 -> read/copy conf_hdd.cfg
 -> unmount
```

Explorer's repeated create/open/stat/enumeration pattern is the real workload Emilia should optimize. Existing CLI instrumentation plus the Chisato 215-read browse baseline provide the lower-level reference.

## Hardware validation progress

Already validated on the real 149.05 GiB APA v2 HDD:

- native System/Light/Dark GUI switching;
- standalone/shared Dokany controller mounting `PhysicalDrive3` as `P:`;
- Explorer root/volume information;
- `Partitions\+OPL` and `Partitions\__common\OPL` enumeration;
- Explorer copy of 20-byte `conf_hdd.cfg` with SHA-256 `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`;
- write creation rejection;
- clean unmount.

Final integrated-GUI gate:

1. start `PS2-DriveForge.exe` as a normal user;
2. confirm one UAC relaunch and no loop;
3. confirm automatic SetupAPI discovery finds the PS2 HDD with no fixed PhysicalDrive list;
4. confirm the sole candidate auto-opens with ~149.05 GiB / APA v2 / 43 main partitions;
5. mount from the GUI and confirm automatic free-letter selection;
6. open in Explorer and browse known PFS paths;
7. confirm writes remain rejected;
8. unmount from the GUI;
9. confirm rescan does not replace an already-open source;
10. once cancel UAC and verify image-capable limited mode plus manual elevation recovery;
11. if practical, occupy `P:` and verify fallback to another free drive letter.

Passing this list is the final blocker before PR #5 is marked ready, squash-merged to `main`, and Darkness development is closed.
