# Darkness 0.4 development plan

Darkness adds a read-only Windows filesystem provider on top of the hardware-validated Chisato stack and finishes the Windows host workflow needed before the 0.5 performance pass.

## Definition of done

A supported PS2 HDD image or physical Windows disk can be discovered, opened, mounted read-only from the native GUI, browsed in Windows Explorer, and unmounted cleanly. Files copied through Explorer must be byte-identical to DriveForge's direct reader/export path. Every source-device operation remains read-only.

The native Windows GUI also supports persistent System/Light/Dark themes with High Contrast passthrough.

**Status: complete and hardware validated.**

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
8. **Done / hardware validated:** SetupAPI `GUID_DEVINTERFACE_DISK` enumeration maps actual Windows disk interfaces to physical-drive numbers through `IOCTL_STORAGE_GET_DEVICE_NUMBER` before the normal DriveForge APA probe.
9. **Done / hardware validated:** fixed visible `PhysicalDrive0..31` GUI list removed; only detected PS2 APA candidates are shown with friendly name, size, APA version, and main-partition count.
10. **Done / hardware validated:** automatic startup discovery plus `Rescan PS2 HDDs`; a single candidate auto-opens only when no source is already open.
11. **Done / hardware validated:** controlled UAC `runas` relaunch with loop prevention, cancellation fallback to image-capable limited mode, and explicit `Restart as Administrator`.
12. **Done / hardware validated:** direct GUI `Mount read-only`, `Open mounted volume in Explorer`, and `Unmount` commands.
13. **Done / hardware validated:** automatic free-drive-letter selection preferring `P:` and avoiding letters below `D:`.
14. **Done:** portable NT `ZwCreateFile` policy regression protects the original root-open bug.
15. **Done:** portable Darkness GUI/mount policy regression records and tests the required one-candidate auto-open and free-letter fallback/exhaustion behavior independently from SetupAPI/Dokany runtime state.
16. **Done:** Windows CI pins/verifies/installs Dokany 2.3.1 SDK/runtime and packages GUI, inspector, mount CLI, docs, and all nine regression tests.
17. **Done / hardware validated:** CLI-driven real-HDD Dokany path validates Explorer browse, known-file copy/hash, write rejection, and clean unmount.
18. **Done / hardware validated:** final integrated GUI elevation/discovery/mount workflow on the real PS2 HDD.

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

```text
normal user
  -> ShellExecuteExW("runas", --elevated-relaunch)
  -> elevated DriveForge
  -> SetupAPI discovery
```

The marker prevents relaunch loops. Cancelling UAC leaves the original GUI running in limited image-capable mode. `Restart as Administrator` can recover raw-disk access later.

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

The adapter now models the NT values explicitly and a portable regression runs under both MSVC and Linux sanitizer CI.

## Windows theme policy

- `View -> Theme -> System / Light / Dark` persists per user.
- System follows Windows' application theme preference.
- High Contrast overrides DriveForge theme choices.
- client area, TreeView, ListView/header, status area, and Windows 11 titlebar receive dark-mode handling;
- optional UxTheme helpers are best-effort only;
- theme code remains above storage/session/parser layers.

## Performance boundary and Emilia handoff

Darkness targets filesystem correctness and stable Explorer behavior. Caching, read-ahead, request coalescing, and overlapped physical I/O are intentionally 0.5 Emilia work.

Preserved baseline workload:

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

Explorer's repeated create/open/stat/enumeration pattern is the real workload Emilia should optimize. Existing backing-I/O counters plus the Chisato 215-read browse baseline provide the lower-level reference.

## Hardware validation conclusion

Darkness is hardware validated on the real 149.05 GiB APA v2 HDD for both the standalone/shared Dokany path and the final integrated GUI path:

- normal-user launch -> one UAC relaunch;
- SetupAPI discovery with no fixed PhysicalDrive list;
- sole PS2 candidate auto-open at ~149.05 GiB / APA v2 / 43 main partitions;
- GUI read-only mount with automatic free-letter selection;
- Explorer browse of known PFS paths;
- bit-identical known-file copy/hash;
- write rejection;
- GUI clean unmount;
- safe rescan behavior;
- cancelled-UAC limited mode and manual elevation recovery;
- preferred-letter fallback behavior.

This closes 0.4 Darkness and hands a stable, measurable Explorer workload to 0.5 Emilia.
