# PS2 DriveForge architecture

## Goal

DriveForge is a Windows-first PS2 HDD management stack with portable APA, PFS, HDL and recovery logic. Format parsing, source I/O, host orchestration, mutation planning, Windows device integration, mounting and presentation are deliberately separate. One frontend should not quietly become a second filesystem implementation just because somebody found a convenient pointer and had a productive afternoon.

The design is intentionally different from wrapping `pfsshell` selected-device/current-mount/current-directory shell state. See [`pfsshell-comparison.md`](pfsshell-comparison.md).

DriveForge also shares a recovery contract with **FHDB Manager**, the PS2-side tool whose repository retains the historical `fhdb-bootstrap-manager` name. The recovery side is documented in [`fhdb-bootstrap-parity.md`](fhdb-bootstrap-parity.md).

## Current Frieren layers

```text
                    Windows presentation

          Win32 GUI                    WinUI 3
              |                           |
              +------ ps2driveforge_host -+
                             |
              DriveSession / ManagementModel
              HDL enrichment / OPL orchestration
                             |
                  ps2driveforge_core
      +-----------+------+------+-------------+
      |           |      |      |             |
     APA         PFS    HDL   recovery     utilities
      |           |      |      |
 ApaVolume  WritableApaVolume  |
      |           |             |
 read capability  |       explicit recovery plans
      |           |             |
 BlockDevice      +------ WritableBlockDevice
      |                         |
 FileBlockDevice         WritableFileBlockDevice
 PhysicalDrive
   read-only
```

Windows discovery, UAC, XAML, theme code, SetupAPI and Dokany remain above the portable layers.

## Capability boundaries

There are three intentionally different storage capabilities:

1. `BlockDevice` exposes byte-addressed reads only.
2. `WritableBlockDevice` adds exact in-place writes and explicit `flush()`.
3. `PhysicalDrive` remains a `BlockDevice` opened read-only on Windows.

`WritableFileBlockDevice` is the current writable backend. It opens an existing image, does not create, truncate or extend it, and exists separately from `FileBlockDevice` so read-only callers do not accidentally inherit mutation power through a refactor.

This distinction is more useful than a boolean named `read_only`. Booleans tend to become `false` eventually, usually five minutes before somebody discovers they selected the wrong disk.

## Dependency rules

1. `ps2driveforge_core` must not depend on Win32 GUI, XAML, SetupAPI, UAC, Explorer or Dokany.
2. PFS accesses APA extents through `ApaVolume` or `WritableApaVolume`; it must not invent physical offsets itself.
3. `ps2driveforge_host` orchestrates sessions, export, management and provider workflows but does not reinterpret on-disk formats.
4. Frontends consume host/core services and snapshots; they do not parse APA/PFS/HDL independently.
5. Windows filename conversion is export policy and must not alter PFS-visible names.
6. `DriveSession` keeps paths explicit and must not become process-global shell state.
7. Caches may memoize data already accepted by normal parser rules. They must not create a faster, weaker parser.
8. Dokany translates Windows filesystem semantics but never becomes another PFS parser.
9. SetupAPI discovers actual Windows disks. Only the APA parser classifies PS2 HDD structure.
10. Normal mutation and exceptional recovery are different trust domains even when both eventually use `WritableBlockDevice`.
11. FHDB Rescue Capsule means only `PS2HBRC\0` v1. `PS2DFRC1` is always a DriveForge Mutation Journal.
12. A physical-disk writer, when it eventually exists, will be a separate capability. The existing `PhysicalDrive` does not grow a write method.

## Offset-based source I/O

`BlockDevice` is byte-addressed. Format code owns conversions among bytes, 512-byte sectors, 1024-byte PFS metadata blocks and PFS zones.

Windows image and physical-disk reads use explicit offsets. `PhysicalDrive` uses overlapped per-request offsets rather than one mutable file pointer. POSIX image reads use `pread()`. The point is correctness first and concurrency second. Shared seek pointers are a charming reminder that threads can turn a simple integer into community property.

## Read cache, read-ahead and instrumentation

The read pipeline is conceptually:

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

Backing counters live below caches so a cache hit removes actual backing I/O instead of merely reporting a suspiciously fast read. Instrumentation records calls, bytes, service time, small reads, failures, largest reads and max in-flight requests.

Storage characteristics such as rotational, solid-state, unknown, seek penalty, TRIM, bus and optional ATA rotation rate are hints. `unknown` is a supported state, not an invitation to guess SSD because optimism is fashionable.

## APA ownership

APA owns physical partition-table interpretation:

```text
1024-byte headers
checksum and Sony master validation
linked-list traversal and cycle detection
main/sub ownership
bounds and extent validation
allocation and removal planning
forensic evidence acquisition
```

APA does not know PFS directory semantics, Windows device discovery or presentation.

### Main/sub relationships

A sub-partition belongs to the main partition identified by its authoritative `main_lba` and `number`. Adjacency and child IDs are presentation clues at best. `group_partition_catalog()` uses the structural relationship and reports orphan subs instead of assigning them to the nearest convenient parent.

The grouped main size is already represented by `Partition::total_sectors`, which includes declared subs. Child extents are summed separately for consistency checks and are not added a second time. Rendering a tree should not manufacture storage capacity.

The same main LBA is the natural deletion identity. `plan_remove_main_partition()` removes a selected main and its owned sub headers as one APA operation.

## ApaVolume and WritableApaVolume

`ApaVolume` translates logical partition/subpart addresses into physical APA extents for read operations:

```text
logical subpart 0 -> APA main extent
logical subpart 1 -> APA child #0
logical subpart 2 -> APA child #1
...
```

`WritableApaVolume` is the write-capable sibling used only after a caller has deliberately obtained `WritableBlockDevice`. It enforces exact sector counts, subpart validity, extent bounds, disk bounds and overflow before forwarding a write.

Keeping this translation in one place prevents every PFS caller from discovering its own exciting interpretation of `BlockInfo.subpart`.

## PFS reader and writer

The PFS reader owns:

```text
superblock
 -> SEGD inode
 -> optional SEGI chain
 -> logical file stream
 -> directory entries
 -> path resolution
```

It validates magic, checksums, zone sizes, descriptors and bounds before exposing results.

Frieren also contains explicit image-only PFS mutation support. Writer modules handle allocation bitmaps, inode publication, directory entries, fragmented/SEGI cases, tree removal, batching and copy-on-write replacement. Mutations operate through `WritableApaVolume`, then cold-read through the normal reader for verification.

The writer does not weaken the reader. A file that can only be understood by a special post-write parser is not a successfully written PFS file. It is a bug with good self-esteem.

## HDL

HDLoader metadata is parsed natively from the main APA partition at `+0x101000` using the `0xDEADFEED` structure. DriveForge does not spawn `HDL.EXE` per game.

Frieren adds:

- bounded HDL metadata patching;
- zero-write APA allocation plans;
- ISO9660 and `SYSTEM.CNF` inspection;
- native DEADFEED/allocation-table generation;
- image-only payload streaming and readback;
- APA publication only after payload verification;
- image-only game removal through APA main/sub ownership.

Bulk ISO bytes are written while planned extents are still unpublished free space. Metadata and APA visibility are committed afterwards. Failure can therefore leave orphan payload bytes in free space, but it does not advertise a half-installed game as valid.

## WriteTransaction

`WriteTransaction` protects known, small, sector-aligned mutation ranges. It:

1. captures exact before-images;
2. rejects overlaps and out-of-bounds ranges;
3. writes staged ranges in order;
4. flushes;
5. reads back exact bytes;
6. runs an optional parser verifier;
7. restores before-images on post-write failure and verifies rollback.

The transaction primitive does not know APA, PFS or HDL rules. Format code decides what bytes are legitimate. A generic transaction layer that also understands every filesystem usually ends up understanding none of them particularly well.

## Mutation Journal

`mutation_journal` is DriveForge-private crash recovery for selected metadata transactions. Its wire magic is `PS2DFRC1` and it records exact before/after ranges plus target size. PREPARED journals can classify a target as all-before, all-after, mixed or foreign/corrupt and can restore exact before-images when appropriate.

It is **not** an FHDB Rescue Capsule. See the shared vocabulary in [`fhdb-bootstrap-parity.md`](fhdb-bootstrap-parity.md).

## FHDB Manager interoperability

Portable bootstrap recovery belongs to the shared FHDB artifact layer:

```text
FHDB Rescue Capsule   PS2HBRC\0 v1
HDDRESCUE*.BIN        master + optional active payload
HDDMBR*.BIN           exact canonical master backup
FHDBMBR*.BIN          accepted legacy master backup names
HDDRAW*.BIN           exceptional raw master snapshot
HDDMETA*.BIN          APAMETA1 touched-header snapshot
FORENSIC.TXT          human forensic report
```

DriveForge mirrors FHDB Manager's two-slot, do-not-overwrite-unrelated-evidence policy for binary recovery artifacts.

### Bootstrap restore

`fhdb_restore` implements image-only bootstrap restore with FHDB Manager semantics:

```text
validate rescue / same disk / live __mbr bounds
 -> freeze exact current master
 -> immediate stale-plan check at apply
 -> mandatory current HDDMBR backup
 -> payload write + flush + exact readback
 -> publish only current osdStart/osdSize + checksum
 -> master write last + flush + exact readback
 -> normal APA parse
```

A corrupt, wrong-disk or invalid full `HDDRESCUE*` blocks fallback to an older legacy header. A valid header-only capsule permits `HDDMBR*` / `FHDBMBR*` pointer fallback. This matches FHDB Manager rather than inventing a host-only recovery personality.

The complete saved master inside a Rescue Capsule is used as identity evidence. It is not blindly copied over the current master during restore.

## APA forensic recovery

Normal APA admission and forensic discovery are intentionally separate. Forensics may inspect structures that the normal reader correctly refuses, but that does not make those structures writable.

The recovery planner/executor preserves these rules:

- incomplete/truncated evidence stays read-only;
- ambiguous or conflicting maps do not auto-authorize repair;
- automatic patches remain narrowly reconstructable;
- stale source bytes refuse before write;
- `HDDRAW` or `HDDMETA` evidence is durable before affected metadata moves;
- interior headers are written before LBA 0;
- every write is flushed and reread;
- final touched-set and normal-parser verification follow the commit.

APA's additive checksum is evidence, not absolution. It can support a narrow reconstruction but cannot magically tell us which field a human, bit flip or ancient tool damaged.

## PartitionCatalog and ManagementModel

`PartitionCatalog` is a pure transformation of one validated `apa::ScanResult`. It records management-relevant rows, sizes and ownership with zero additional device I/O.

`ManagementModel` consumes that snapshot and tracks progressive HDL enrichment. Surviving enrichment is preserved when a known committed removal deletes one main/sub group from the in-memory model. A successful deletion therefore does not require re-reading every unrelated game just to reassure the UI that they still exist.

## DriveSession

`DriveSession` owns one opened read source and reusable frontend operations:

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

It remains a read session. Mutation coordinators may update its cached snapshot after a separately committed known plan, but they do not turn `DriveSession` into the write authority.

## OPL asset pipeline

OPL provider fetching and PFS destination mutation are separate stages. Provider code resolves title/Redump/CFG/compatibility/CHT/mastercode/artwork data into host staging with provenance. Destination code then plans and writes selected assets through PFS mutation.

Network data never receives a direct `WritableApaVolume`. The Internet is useful enough without giving arbitrary HTTP responses sector access.

## Windows discovery and elevation

DriveForge enumerates Windows disks through SetupAPI, resolves the actual `PhysicalDriveN`, opens it read-only and lets the APA parser determine whether it is a PS2 HDD.

Disk images do not need administrator rights. Raw disk reads often do. Current Windows frontends handle UAC separately from format logic.

A future physical mutation backend must re-prove source identity immediately before commit and pass the physical-write gates. It will not modify the existing read-only `PhysicalDrive` contract.

## Dokany boundary

Dokany sits above `ReadOnlyMountView` and `DriveSession`. It translates Windows filesystem behavior but cannot mutate the source.

Read-only protection is layered:

```text
BlockDevice has no write()
PhysicalDrive uses GENERIC_READ
DOKAN_OPTION_WRITE_PROTECT
mutation/create/delete paths are rejected
```

### NT create-disposition trap

Dokany `ZwCreateFile` receives NT `FILE_*` dispositions, not Win32 `CreateFileW` creation constants. Numeric overlap once produced Explorer's historical `The file exists` root-open failure. The policy is isolated and tested. Do not simplify the constants because two Microsoft APIs happened to use integers and optimism.

## Frontends

The Win32 frontend is the validated normal Windows entrypoint and consumes shared services for browsing, management and mounting.

The WinUI frontend remains a parity path rather than an independent storage implementation. Partition presentation now groups main partitions with collapsible authoritative subpartitions, exposes logical total sizes and isolates orphan subs diagnostically.

Frontend code may decide how to explain a recovery plan. It may not decide which sectors belong in one.

## Build and release boundary

CMake owns portable/native libraries, CLI, Win32 and Dokany targets. WinUI remains MSBuild/C++WinRT because Windows App SDK tooling lives there.

`scripts/frieren-regression-tests.ps1` is the canonical Windows regression executable manifest shared by staging and package verification. The number of tests should therefore be derived from the manifest rather than duplicated in documentation every time another safety case acquires opinions.

See [`../BUILDING.md`](../BUILDING.md), [`testing.md`](testing.md) and [`release-process.md`](release-process.md).

## Address-unit warning

When debugging storage, write the unit at every boundary:

```text
host bytes
 <-> 512-byte PS2 sectors
 <-> 1024-byte PFS metadata blocks
 <-> PFS zones
 <-> APA logical extents
 <-> physical LBA
```

Typical mistakes include treating `BlockInfo.number` as one universal unit, applying payload-zone arithmetic to metadata, pre-adding APA starts before `ApaVolume`, assuming subpartitions are contiguous, or confusing `osdSize` sectors with payload bytes. Storage code is sufficiently unforgiving without unit ambiguity lending a hand.

## Historical real-HDD performance evidence

The 2026-08-22 Emilia validation sweep remains a useful baseline for the read path. It records one specific 149.05 GiB APA v2 HDD and should not be mistaken for a law of nature. See [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md).

## Debugging starting points

| Symptom | First layer |
| --- | --- |
| APA not detected or chain stops | `src/core/apa.cpp` |
| extent outside device | APA bounds diagnostics |
| PFS read parse failure | `src/core/pfs.cpp` |
| PFS mutation failure | PFS writer / `WritableApaVolume` |
| wrong main/sub physical data | `ApaVolume` / `WritableApaVolume` |
| HDL title/startup corrupt | `src/core/hdl.cpp` |
| game install publication failure | `hdl_image_install.cpp` / APA mutation |
| main/sub UI ownership wrong | `partition_grouping` / catalog data |
| Rescue Capsule rejected | `fhdb_rescue` / `fhdb_restore` |
| legacy FHDBMBR not selected | `fhdb_restore` discovery policy |
| Mutation Journal state unclear | `mutation_journal` |
| forensic topology ambiguous | `apa_forensic` |
| recovery write refused | recovery plan source-stability / artifact gate |
| export-name/path failure | `pfs_export.cpp` |
| GUI/CLI browse disagreement | `DriveSession` inputs/results |
| Explorer says `The file exists` | NT Dokany open policy |
| PS2 HDD missing from GUI | SetupAPI / UAC / raw-open diagnostics |
| unexpected backing I/O | cache/read-ahead/instrumentation |
| Windows package misses a regression EXE | Frieren regression manifest / package verifier |

Identify the wrong abstraction before adding compatibility hacks above it. Compatibility hacks are remarkably good at surviving the bug that created them.
