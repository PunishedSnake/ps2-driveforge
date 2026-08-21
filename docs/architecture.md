# PS2 DriveForge architecture

## Goal

Build a Windows-first PS2 HDD management stack that can expose an APA disk to Windows Explorer while keeping parsing, host operations, discovery, mounting, and presentation independent.

The architecture is intentionally different from wrapping pfsshell's interactive device/mount/current-directory model. See [`pfsshell-comparison.md`](pfsshell-comparison.md).

## Current layers

```text
 Native Win32 GUI          CLI / mount CLI       Dokany callbacks
        |                         |                      |
        +------------- host/session/mount --------------+
                                  |
                            DriveSession
                                  |
                         ps2driveforge_core
                                  |
                      +-----------+-----------+
                      |                       |
                     APA                     PFS
                      |                       |
                      +-----------+-----------+
                                  |
                      InstrumentedBlockDevice
                                  |
                            BlockDevice
                          /             \
                    disk image      PhysicalDriveN
```

`DriveSession` is frontend orchestration, not filesystem-global state. `InstrumentedBlockDevice` is a transparent diagnostic wrapper, not another on-disk abstraction.

### BlockDevice

`BlockDevice` is byte-addressed so format code owns sector/alignment semantics rather than leaking Windows handles or iomanX conventions upward.

Current backends are read-only:

- `FileBlockDevice`;
- Windows `PhysicalDrive` opened with `GENERIC_READ` only.

The absence of `write()` is a safety boundary. Future mutation must introduce a separate explicit writable capability.

### APA / ApaVolume

APA owns physical partition-table interpretation: 1024-byte header parsing/checksum, linked-list traversal, main/sub metadata, diagnostics, and extent bounds.

`ApaVolume` is the only layer translating PFS logical subpart indices into physical APA extents:

```text
PFS subpart 0 -> APA main extent
PFS subpart 1 -> APA sub extent 0
PFS subpart 2 -> APA sub extent 1
...
```

PFS code must not pre-add physical APA LBAs itself.

### PFS

The PFS reader owns:

```text
superblock
 -> SEGD inode
 -> optional SEGI chain
 -> logical byte stream
 -> directory entries
 -> path resolution
```

It does not own Windows filename rules, SetupAPI, UAC, Explorer, or Dokany.

### DriveSession / host layer

`DriveSession` owns one opened source plus reusable frontend operations:

```text
scan
find partition
browse(partition, path)
stat(partition, path)
read_file(partition, path, offset, span)
export_to_host(...)
statistics
```

Paths remain explicit. GUI navigation state stays in the GUI. Dokany resolves independent callback paths through the same session model.

The host layer additionally owns recursive export, host filename conversion, collision/cycle/depth policy, Windows discovery, and portable `ReadOnlyMountView` path mapping.

## Windows disk discovery boundary

Darkness no longer guesses a visible `PhysicalDrive0..31` range.

```text
SetupAPI GUID_DEVINTERFACE_DISK
 -> real disk interface
 -> IOCTL_STORAGE_GET_DEVICE_NUMBER
 -> PhysicalDriveN
 -> GENERIC_READ
 -> APA parser
```

The interface handle used for device-number lookup requests no disk data access. Device model/capacity are presentation metadata only. **APA validation is the authority for identifying a PS2 HDD.**

Discovery must never evolve into an implicit writable-target selector.

## Elevation boundary

Raw-disk access commonly needs Administrator privileges, but image browsing does not. Therefore DriveForge does not use a global `requireAdministrator` manifest.

The Win32 GUI owns controlled `ShellExecuteExW("runas")` relaunch, loop prevention, cancellation fallback, and the manual `Restart as Administrator` action. Core/host format logic does not know about process tokens or UAC.

## Darkness mount boundary

`DokanyMountController` is the single runtime owner of Dokany callbacks and mount lifecycle.

```text
Native GUI                 mount CLI
    |                         |
    +--- DokanyMountController+
                 |
          ReadOnlyMountView
                 |
            DriveSession
```

The standalone console frontend remains useful for scripts and callback logging, but it does not own a second filesystem implementation and the GUI does not spawn it as a helper process.

The provider has four independent read-only barriers:

```text
no BlockDevice::write()
        +
PhysicalDrive GENERIC_READ
        +
DOKAN_OPTION_WRITE_PROTECT
        +
mutation/create/overwrite rejection
```

### NT create disposition rule

Dokany's `ZwCreateFile` receives NT kernel `FILE_*` dispositions, not Win32 `CreateFileW` constants. The original real mount exposed the overlap (`FILE_OPEN == 1` vs Win32 `CREATE_NEW == 1`) as Explorer's **"The file exists."** root failure.

That contract is isolated in `src/mount/dokany_open_policy.hpp` and protected by a portable regression test.

### Darkness GUI/mount selection policy

`include/ps2hdd/darkness_policy.hpp` records two small rules that are easy to regress during UI refactors:

- startup discovery may auto-open only when exactly one PS2 candidate exists and no source is already open;
- automatic mount letters prefer `P:`, then other free data letters, and never select below `D:`.

A deterministic portable regression tests these policies without requiring SetupAPI, Dokany, or a specific runner drive layout.

## Theme boundary

System/Light/Dark behavior remains entirely in the native GUI layer. DWM/UxTheme/registry behavior does not enter DriveSession, mount namespace logic, or APA/PFS code.

## Release-train map

### 0.1 Ayanami

APA read-only core — implemented and hardware validated.

### 0.2 Bocchi

PFS read path — implemented and hardware validated; real PFS SEGI remains an explicit coverage gap protected by generated-image E2E.

### 0.3 Chisato

Native Windows browser, shared host/session/export layer, instrumentation and parser hardening — implemented and hardware validated.

### 0.4 Darkness

Read-only Explorer filesystem and integrated Windows device workflow:

- portable mount namespace;
- random-offset/thread-safe session operations;
- Dokany 2.3.1 provider;
- layered write protection;
- native System/Light/Dark GUI;
- SetupAPI device discovery;
- controlled UAC elevation;
- shared GUI/CLI mount controller;
- direct GUI mount/open/unmount;
- nine-test regression suite;
- standalone CLI-driven real-HDD mount hardware validated;
- final integrated-GUI hardware gate pending.

## Dependency rules

1. `ps2driveforge_core` must not include Win32 GUI/Dokany/host-path policy.
2. PFS accesses physical data through `ApaVolume`.
3. Frontends must not reimplement APA/PFS parsing.
4. Windows filename conversion must not alter PFS-visible names.
5. Read-only parsing must remain usable without GUI/Dokany.
6. `DriveSession` must not become shell-global current-directory state.
7. Instrumentation/caches must remain transparent to parser correctness.
8. Dokany policy may translate NT semantics but not parse the filesystem.
9. SetupAPI/UAC/theme behavior stays above storage format logic.
10. Future write support must be a separate capability with backup/recovery semantics.

## Address-unit warning

When debugging PFS, write down the unit at each step:

```text
host bytes
 <-> 512-byte PS2 sectors
 <-> 1024-byte PFS metadata blocks
 <-> PFS zones
 <-> APA logical extents
 <-> physical LBA
```

Common mistakes include treating `BlockInfo.number` as the same unit everywhere, using payload-zone arithmetic for metadata, pre-adding APA starts before `ApaVolume`, or assuming sub-partitions are physically contiguous.

See [`apa-format-notes.md`](apa-format-notes.md) and [`pfs-format-notes.md`](pfs-format-notes.md).

## Current performance reality

Correctness is ahead of throughput:

```text
DriveSession
 -> InstrumentedBlockDevice
 -> PhysicalDrive::read
 -> per-device mutex
 -> SetFilePointerEx
 -> synchronous ReadFile
```

Explorer also performs repeated open/stat/enumeration operations. Darkness preserves this behavior as the baseline. 0.5 Emilia owns inode/directory/block caches, read-ahead, request coalescing, and overlapped physical I/O. See [`performance.md`](performance.md).

## Explorer namespace

```text
P:\
  Partitions\
    __system\
    __common\
    +OPL\
  Games\                # later
  System\               # later
```

Only `Partitions` is active in Darkness.

## Debugging starting points

| Symptom | First layer |
| --- | --- |
| APA not detected / chain stops | `apa.cpp` |
| extent outside device | APA bounds diagnostics |
| PFS superblock invalid | `pfs::probe()` |
| inode checksum/magic failure | PFS metadata addressing |
| wrong payload with valid inode | zone arithmetic / SEGD-SEGI traversal |
| only one sub-part fails | `ApaVolume` |
| export-name/path failure | host exporter |
| GUI/CLI browse disagreement | `DriveSession` inputs/results |
| Explorer says `The file exists` on normal open | NT Dokany create disposition policy |
| direct browse works but mount path fails | `ReadOnlyMountView` / Dokany adapter |
| PS2 HDD missing from GUI | SetupAPI discovery / UAC / raw-open error |
| mount chooses wrong/occupied letter | Darkness mount-letter policy |
| slow Explorer workload | instrumentation + `performance.md` |

Identify the wrong abstraction before adding compatibility hacks above it.
