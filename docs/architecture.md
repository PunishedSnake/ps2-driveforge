# PS2 DriveForge architecture

## Goal

DriveForge is a Windows-first PS2 HDD management stack with a portable read-only APA/PFS/HDL core. Format parsing, source I/O, host/session orchestration, Windows device integration, mounting, and presentation are deliberately separate so one frontend cannot quietly become a second filesystem implementation.

The design is intentionally different from wrapping `pfsshell`'s selected-device/current-mount/current-directory shell state. See [`pfsshell-comparison.md`](pfsshell-comparison.md).

## Current Emilia layers

```text
                 Windows presentation

  legacy Win32 GUI              WinUI 3 C++/WinRT
        |                              |
        |                    NativeSessionController
        |                              |
        +------------- ps2driveforge_host -------------+
                              |          |              |
                         DriveSession    |       ManagementModel
                              |    PartitionCatalog      |
                              |          |        HDL enrichment
                              +----------+--------------+
                                         |
                              ps2driveforge_core
                              /      |       \
                            APA     PFS      HDL
                              \      |       /
                               ApaVolume
                                  |
                        read-only I/O pipeline
             cache -> read-ahead -> instrumentation
                                  |
                    FileBlockDevice / PhysicalDrive

  CLI / benchmark --------------^                ^------ Dokany adapter
```

Windows discovery, UAC, theme resources, XAML and Dokany are above these portable layers.

## Dependency rules

1. `ps2driveforge_core` must not depend on Win32 GUI, XAML, SetupAPI, UAC, Explorer or Dokany.
2. PFS accesses physical APA extents through `ApaVolume`; it must not pre-add physical partition starts itself.
3. `ps2driveforge_host` may orchestrate sessions/export/management state but must not reimplement format parsing.
4. Frontends consume host/core services and snapshots; they do not parse APA/PFS/HDL independently.
5. Windows filename conversion is host-export policy and must not alter PFS-visible names.
6. `DriveSession` keeps paths explicit and must not become process-global shell/current-directory state.
7. Caches may memoize already validated read-only data; they must not create a weaker parser path.
8. Dokany translates Windows filesystem semantics but does not parse the source filesystem.
9. SetupAPI identifies actual Windows disk devices; only the APA parser decides whether a disk is a PS2 HDD.
10. Future source mutation requires a separate explicit writable capability with backup/recovery semantics.

## Core: BlockDevice and source I/O

`BlockDevice` is byte-addressed. Format code owns sectors/zones/metadata-block conversions rather than leaking Windows handles or iomanX conventions upward.

Current source backends are read-only:

- `FileBlockDevice` for images;
- Windows `PhysicalDrive` opened with `GENERIC_READ` only.

The absence of a source `write()` member is an architectural safety boundary, not merely a UI policy.

### Emilia offset-based I/O

Darkness' old shared seek-pointer model is no longer current architecture.

Windows physical disks and image files use explicit request offsets. `PhysicalDrive` uses `FILE_FLAG_OVERLAPPED`/per-request offsets rather than `SetFilePointerEx` plus one shared file-position mutex. Windows image I/O follows the same offset model; POSIX image I/O uses `pread()`.

This allows measured concurrency without making correctness depend on mutable file-position state.

## Core: read cache, read-ahead and instrumentation

The source pipeline is conceptually:

```text
format/session reads
     |
small-read window cache
     |
adaptive sequential read-ahead
     |
InstrumentedBlockDevice
     |
actual image / PhysicalDrive
```

Backing counters sit below the cache layers. A cache hit therefore disappears from physical/image read-call and byte totals rather than being counted as an I/O that merely completed quickly.

The instrumentation records backing calls/bytes, service time, small reads, largest read, failed reads and max in-flight requests. Cache/read-ahead layers expose their own usefulness counters.

Storage characteristics (`rotational`, `solid-state`, `unknown`, seek penalty, TRIM, bus, optional ATA rotation rate) are hints. `unknown` is a normal state and must not be coerced into one tuning preset based on a single validation disk.

## Core: APA

APA owns physical partition-table interpretation:

- 1024-byte header parsing/checksum;
- linked-list traversal/cycle detection;
- main/sub metadata;
- diagnostics and device bounds;
- raw type/flags/start/length relationships.

APA does not know PFS directory semantics, Windows device discovery or presentation.

## Core: ApaVolume

`ApaVolume` is the only layer translating PFS logical subpart indices into APA physical extents:

```text
PFS subpart 0 -> APA main extent
PFS subpart 1 -> APA sub extent 0
PFS subpart 2 -> APA sub extent 1
...
```

This boundary prevents unit/addressing bugs from being duplicated in PFS callers.

## Core: PFS

The PFS reader owns:

```text
superblock
 -> SEGD inode
 -> optional SEGI chain
 -> logical byte stream
 -> directory entries
 -> path resolution
```

It validates checksums/magic/ranges before caching results. PFS does not know Windows filename policy, SetupAPI, UAC, Explorer or Dokany.

`DriveSession` retains immutable validated probe/node/directory/stat results for the lifetime of one source session. `clear_caches()` creates a deliberate cold boundary for benchmarks/source changes; warm repeated metadata walks can otherwise become zero-backing-I/O operations.

## Core: native HDL metadata

HDLoader game metadata is parsed natively from the main APA partition at `+0x101000` using the `0xDEADFEED` header. DriveForge does not launch `HDL.EXE` per game.

The parser is in `src/core/hdl.cpp`; its public types/constants remain in `include/ps2hdd/hdl.hpp`. A single game enrichment performs one bounded metadata read after validating partition/device ranges.

Baseline catalog ordering is ascending physical LBA, which avoids arbitrary head movement on rotational/unknown storage and remains harmless on SSDs.

## Zero-I/O PartitionCatalog

`PartitionCatalog` is a pure transformation of an already validated `apa::ScanResult`:

```text
one APA scan
 -> build_partition_catalog()
 -> complete rows/counts/sizes/relationships
 -> zero additional source I/O
```

It records all management-relevant APA facts without probing PFS or HDL payloads. Showing/hiding subpartitions, sorting, filtering and selecting catalog rows must remain memory-only frontend operations.

Implementation lives in `src/core/partition_catalog.cpp`; the public header carries only the data contract/declarations.

## Host: DriveSession

`DriveSession` owns one opened source and the reusable operations required by CLI/GUI/mount/benchmark code:

```text
scan
partition_catalog
find partition
browse(partition, path)
stat(partition, path)
read_file(partition, path, offset, span)
export_to_host(...)
statistics / cache reset
```

Navigation state stays in the frontend. `DriveSession` is orchestration, not shell-global state.

## Host: ManagementModel and enrichment

`ManagementModel` consumes `PartitionCatalog` without touching the disk. Base rows exist immediately. Main HDL rows begin as `pending`; progressive enrichment applies native `GameResult` objects one row at a time and tracks pending/ready/failed counts.

The model implementation belongs in `src/host/management_model.cpp` because it is frontend-neutral orchestration, not on-disk format parsing.

The production HDL enrichment scheduler is also host policy. Current policy is conservative for rotational/unknown media; positively identified storage can use bounded concurrency where measurements justify it. The benchmark `--hdl-qd N` switch is a developer override, not frontend policy.

## Windows discovery boundary

DriveForge does not scan an arbitrary visible `PhysicalDrive0..31` range.

```text
SetupAPI GUID_DEVINTERFACE_DISK
 -> actual disk interface
 -> IOCTL_STORAGE_GET_DEVICE_NUMBER
 -> PhysicalDriveN
 -> GENERIC_READ
 -> normal APA parser
```

Device model/capacity/bus are presentation/tuning metadata only. **APA validation is the authority for PS2 HDD identity.**

The physical-drive number is transient Windows enumeration state and may change after reboot/reconnection.

## Elevation boundary

Disk-image browsing does not need administrator rights. Raw Windows disk access often does.

The validated Win32 frontend uses controlled `ShellExecuteExW("runas")` relaunch, loop prevention, UAC-cancel fallback to image-capable mode and explicit `Restart as Administrator`.

The long-term WinUI design narrows this further:

```text
WinUI shell (normal user)
      |
read-only IPC
      |
elevated raw-disk broker
      |
GENERIC_READ PhysicalDrive
```

The broker is a target architecture, not something the current WinUI preview is allowed to pretend already exists.

## Dokany mount boundary

`DokanyMountController` is the runtime owner of Dokany callbacks/lifecycle.

```text
Win32 GUI / mount CLI
          |
DokanyMountController
          |
ReadOnlyMountView
          |
DriveSession
```

The standalone `PS2-DriveForge-Mount.exe` remains a thin diagnostics/script frontend over the same controller.

Read-only protection is layered:

```text
no source BlockDevice::write()
        +
PhysicalDrive GENERIC_READ
        +
DOKAN_OPTION_WRITE_PROTECT
        +
create/mutation/overwrite/delete rejection
```

### NT create-disposition trap

Dokany's `ZwCreateFile` receives NT `FILE_*` disposition values, not Win32 `CreateFileW` constants. Numeric overlap originally produced Explorer's historical `The file exists` root-open failure.

The policy is isolated in `src/mount/dokany_open_policy.hpp` and covered by a portable regression test. Do not "simplify" these values into Win32 constants.

## Frontends

### Legacy Win32

The Win32 frontend is the currently validated normal Windows entrypoint. It owns Windows presentation, theme, UAC orchestration, source selection/navigation, export actions and mount controls. It consumes shared storage/host services.

Large legacy frontend files are a cleanup target, but source splitting must preserve behavior and should be done only with MSVC + real-Windows regression coverage rather than as cosmetic churn immediately before an RC.

### WinUI 3

The unpackaged/self-contained WinUI 3 C++/WinRT project uses Windows App SDK 2.3.1. `NativeSessionController` bridges the native source/session/catalog/enrichment model into immutable frontend snapshots.

Current status is **preview/parity work**, not production-default parity. The visual shell and native session/catalog bridge build/package successfully; complete disk-image/PFS/export/mount workflows and the least-privilege raw-disk broker remain work.

Until parity is proven, the canonical package carries WinUI under `WinUI\` beside the validated Win32 fallback.

## Build/release boundary

CMake owns portable/native libraries, CLI, Win32 and Dokany targets. The WinUI project remains MSBuild/C++WinRT because XAML/Windows App SDK build tooling is MSBuild-oriented.

`build-windows.ps1` composes the full release payload. `scripts/verify-windows-package.ps1` validates canonical staging independently of whatever MSBuild subdirectory happens to contain the WinUI EXE.

See [`../BUILDING.md`](../BUILDING.md) and [`release-process.md`](release-process.md).

## Address-unit warning

When debugging PFS, write the unit at every boundary:

```text
host bytes
 <-> 512-byte PS2 sectors
 <-> 1024-byte PFS metadata blocks
 <-> PFS zones
 <-> APA logical extents
 <-> physical LBA
```

Common mistakes include treating `BlockInfo.number` as the same unit everywhere, applying payload-zone arithmetic to metadata, pre-adding APA starts before `ApaVolume`, or assuming subpartitions are physically contiguous.

## Current real-HDD performance evidence

The preserved 2026-08-22 validation sweep records, on one 149.05 GiB APA v2 disk:

- cold APA scan median 1555.022 ms for 190 headers/reads;
- zero-I/O catalog median 0.006 ms for 190 rows;
- cold `+OPL /` browse+stat: 10 reads / 16 KiB / 41.493 ms median;
- immediate warm repeat: 0 backing reads / 0.001 ms median;
- 35/35 native HDL metadata rows readable;
- unknown-media automatic policy remains QD1 despite QD8 winning the narrow total-completion benchmark, because QD8 multiplied per-read latency and base rows are already usable before enrichment.

These measurements support the architecture but do not define universal tuning for other HDDs, SSDs or bridges. See [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md).

## Debugging starting points

| Symptom | First layer |
| --- | --- |
| APA not detected / chain stops | `src/core/apa.cpp` |
| extent outside device | APA bounds diagnostics |
| PFS superblock/inode/dentry invalid | `src/core/pfs.cpp` |
| wrong main/sub physical data | `ApaVolume` |
| HDL title/startup corrupt | `src/core/hdl.cpp` |
| list creation causes payload I/O | `partition_catalog.cpp` / caller |
| progressive game row wrong | `ManagementModel` / `hdl_enrichment.cpp` |
| export-name/path failure | `pfs_export.cpp` |
| GUI/CLI browse disagreement | `DriveSession` inputs/results |
| Explorer says `The file exists` | NT Dokany open policy |
| direct browse works but mount fails | `ReadOnlyMountView` / Dokany adapter |
| PS2 HDD missing from GUI | SetupAPI / UAC / raw-open diagnostics |
| wrong mount letter | Darkness mount-letter policy |
| unexpected physical I/O | session cache + read cache/read-ahead + instrumentation |
| WinUI build packages wrong path | build script / canonical package verifier |

Identify the wrong abstraction before adding compatibility hacks above it.
