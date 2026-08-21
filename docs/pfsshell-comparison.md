# DriveForge and pfsshell: architecture comparison

This document explains why PS2 DriveForge exists when `pfsshell` already provides mature APA/PFS access.

The goal is not to diminish `pfsshell`. It remains an important compatibility/format reference and its tree also contains `pfsfuse`. DriveForge solves a different host-side problem: a Windows-first, library-first PS2 HDD stack for a native GUI, Explorer filesystem, diagnostics, and eventually carefully gated mutation without exposing PS2SDK/iomanX process state to every frontend.

## Ground rules

Claims are split into:

- **Implemented / observed** — behavior present in DriveForge or directly visible in referenced pfsshell source.
- **Design target** — an architectural opportunity that must not be described as a measured speedup until benchmarks exist.

"Faster than pfsshell" without workload, device, connection, build/version, cache state, and measurements is not an acceptable project claim.

## pfsshell model

The current pfsshell workflow is naturally shell-oriented:

```text
select device
 -> mount partition
 -> maintain current PFS directory
 -> ls/get/put/etc.
 -> unmount
```

The shell maintains device/mount/current-path state and routes operations through its host iomanX adapter plus ported APA/PFS/HDL layers. This is a sensible compatibility design because it reuses PS2-side semantics.

`pfsfuse` provides a filesystem frontend separately. The pfsshell documentation also describes Windows use through a Dokan FUSE wrapper, mounting a selected PFS partition to a drive letter.

References:

- <https://github.com/ps2homebrew/pfsshell>
- <https://github.com/ps2homebrew/pfsshell/blob/master/README.md>
- <https://github.com/ps2homebrew/pfsshell/blob/master/src/shell.c>
- <https://github.com/ps2homebrew/pfsshell/blob/master/src/iomanx_adapter.c>

## DriveForge model

DriveForge keeps host orchestration explicit rather than reproducing shell-global mount/current-directory state:

```text
Native GUI                CLI / mount CLI
    |                            |
    +------- shared host/session/mount -------+
                        |
                 DokanyMountController
                        |
                 ReadOnlyMountView
                        |
                   DriveSession
                        |
                    PFS Reader
                        |
                    ApaVolume
                        |
                    APA parser
                        |
             InstrumentedBlockDevice
                        |
                   BlockDevice
                 /             \
           disk image      PhysicalDriveN
```

A `DriveSession` accepts explicit partition/path/range operations. GUI navigation stays in the GUI. Dokany callbacks resolve their paths independently. APA physical placement stays in `ApaVolume`.

## Implemented differences useful for our target

These are architecture/functionality statements, not throughput benchmarks.

### 1. Read-only safety is structural

Current physical disks are `GENERIC_READ` only and `BlockDevice` has no write method. Darkness adds `DOKAN_OPTION_WRITE_PROTECT` and explicit rejection of create/overwrite/mutation callbacks.

Write support will require a new explicit capability plus backup/recovery rules rather than widening the reader silently.

### 2. APA extents are normalized/validated once

PFS logical `(subpart, address)` access is translated through `ApaVolume`. APA also rejects recorded main/sub extents outside the backing device before PFS receives them.

This avoids leaking physical placement assumptions into file/directory code.

### 3. Byte-range reads are first-class

Mounted Windows reads can begin/end at arbitrary byte offsets. DriveForge's PFS reader presents one logical byte stream across SEGD/SEGI descriptors and APA extents rather than requiring the frontend to emulate PS2-sector-aligned shell operations.

### 4. SEGI traversal is hidden behind the reader

Callers do not understand direct/indirect descriptor mechanics. Generated-image E2E validates a real SEGI-backed file and APA main/sub crossing even though the current physical test HDD lacks a suitable large PFS payload.

### 5. Host policy is not parser policy

Windows reserved names, filename conversion, case-insensitive collisions, recursive host export, SetupAPI discovery, UAC, theme behavior, and Dokany are above `ps2driveforge_core`.

PS2-visible names and on-disk interpretation remain format concerns.

### 6. Cross-layer behavior is reproducible

The test suite generates a real sparse APA/PFS `.img`, reopens it through production `FileBlockDevice`, traverses APA/PFS, recursively exports data, and verifies SHA-256.

Malformed metadata has a deterministic corruption corpus. Frontend contracts such as NT Dokany dispositions and Darkness auto-open/free-letter policy also have portable regressions.

### 7. Explorer integration now uses the same stack

Darkness' real HDD path has hardware-validated:

```text
PhysicalDrive GENERIC_READ
 -> APA/PFS
 -> DriveSession
 -> ReadOnlyMountView
 -> DokanyMountController
 -> Explorer
```

Root/partition browsing works, a known real file copied through Explorer matches its expected SHA-256, write creation is rejected, and unmount is clean.

The GUI and standalone mount CLI now share `DokanyMountController`; the CLI is retained for diagnostics/scripts rather than becoming a second implementation.

### 8. Windows disk selection no longer means guessing PhysicalDrive numbers

The Darkness GUI enumerates actual `GUID_DEVINTERFACE_DISK` devices through SetupAPI, maps the device interface to the real raw-disk number, then lets the DriveForge APA parser classify it.

Model/capacity are display metadata only. Windows device discovery is not used as a filesystem-format heuristic.

## Where DriveForge is *not* faster yet

Current physical Windows I/O is still deliberately conservative:

```text
PFS Reader
 -> ApaVolume
 -> InstrumentedBlockDevice
 -> PhysicalDrive
 -> mutex
 -> SetFilePointerEx
 -> synchronous ReadFile
```

There is currently:

- no inode metadata cache;
- no directory cache;
- no block/read-window cache;
- no read-ahead;
- no request coalescing beyond current reader batching;
- no overlapped raw-disk I/O;
- no benchmark proving throughput superiority over pfsshell/pfsfuse.

Explorer has also demonstrated a highly repetitive open/stat/enumeration workload, so a correct Dokany mount alone is not a performance victory.

## Emilia performance path

0.5 should optimize the preserved Darkness workload rather than replacing Dokany or introducing a custom kernel filesystem.

Planned order:

1. add logical/metadata/payload/timing counters;
2. cache validated immutable inode metadata;
3. cache directory results;
4. add block/read-window caching;
5. coalesce adjacent validated reads;
6. adaptive sequential read-ahead;
7. offset/overlapped Windows physical reads;
8. safe concurrent scheduling;
9. repeat Darkness Explorer and export workloads;
10. compare with pfsshell/pfsfuse only under controlled conditions.

See [`performance.md`](performance.md).

## Explorer namespace

Darkness implements the first part of the broader namespace target:

```text
PS2HDD (P:)\
  Partitions\
    __system\
    __common\
    +OPL\
  Games\              # later, HDL/Guts
  System\             # later, metadata/recovery
```

Only `Partitions` is active in 0.4.

## Development rule

Whenever a change is called "faster" or "better than pfsshell":

1. identify the specific old UX/bottleneck;
2. identify the DriveForge layer that changes it;
3. add/retain correctness coverage;
4. use counters and timings for speed claims;
5. update this document with measured results, not assumptions.

That distinction is intended to keep DriveForge understandable after the original implementation context is gone.
