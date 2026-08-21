# DriveForge and pfsshell: architecture comparison

This document explains why PS2 DriveForge exists when `pfsshell` already provides mature APA/PFS access.

The goal is not to diminish `pfsshell`. It is an important compatibility and format reference, remains useful, and its current tree also contains `pfsfuse`. DriveForge is deliberately solving a different host-side problem: a Windows-first, library-first PS2 HDD stack that can support a GUI, an Explorer filesystem provider, diagnostics, and eventually safe mutation without exposing PS2SDK/iomanX process state to every frontend.

## Ground rules

Claims in this document are split into two categories:

- **Implemented / observed**: behavior present in DriveForge today or directly visible in the referenced pfsshell source.
- **Design target**: an architectural advantage we expect to exploit, but which must not be described as a measured speedup until a benchmark exists.

Performance claims must always include the workload, device, connection method, build, and comparison version. "Faster than pfsshell" without those details is not an acceptable project claim.

## pfsshell model

The current pfsshell README describes an interactive workflow:

```text
select device
    -> mount partition
        -> maintain current PFS directory
            -> ls/get/put/etc.
        -> unmount
```

The shell source keeps this state explicitly in a context containing device setup state, a mounted partition, a mount point, and the current path. Operations are routed through the host port of iomanX and the ported APA/PFS/HDL modules.

This is a sensible compatibility architecture: it reuses semantics that already exist in PS2 software and exposes them through a command shell. It also means the host frontend inherits concepts such as a selected device, a mounted `pfs0:` instance, current-directory state, and iomanX-style operations.

`pfsfuse` provides a filesystem frontend separately. On Windows, pfsshell documents the Dokan FUSE wrapper and an invocation that mounts a selected PFS partition to a drive letter.

References:

- <https://github.com/ps2homebrew/pfsshell>
- <https://github.com/ps2homebrew/pfsshell/blob/master/README.md>
- <https://github.com/ps2homebrew/pfsshell/blob/master/src/shell.c>
- <https://github.com/ps2homebrew/pfsshell/blob/master/src/iomanx_adapter.c>

## DriveForge model

DriveForge deliberately moves the host abstraction boundary lower and now shares frontend orchestration explicitly:

```text
GUI / CLI / future Dokany provider
              |
         DriveSession
              |
       host operations
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

There is no global "currently mounted PFS" concept in the core and no core current-directory state. A `pfs::Reader` belongs to one `ApaVolume`; `DriveSession` accepts explicit partition/path arguments; GUI navigation state remains a frontend concern.

This gives frontends a model closer to native desktop/filesystem-provider software:

- multiple volumes can eventually exist at the same time;
- directory navigation is frontend state, not filesystem-global state;
- a file read is `Reader + Node + offset + span`, not an iomanX file descriptor hidden behind a process-wide mount;
- APA main/sub-partition translation lives in `ApaVolume`, so PFS callers do not know physical extent placement;
- GUI and CLI use the same `DriveSession` browse/export path;
- host export is a separate layer above the parser rather than part of filesystem parsing;
- a future Dokany provider can call explicit read-oriented primitives directly instead of emulating an interactive shell session.

## What is already better for our target use case

These are functionality/architecture statements, not throughput benchmarks.

### 1. Read-only safety is structural

Current DriveForge physical disks are opened with `GENERIC_READ`. The public `BlockDevice` interface has no write method. A parser bug therefore does not have a normal code path to modify the PS2 disk.

Before write support is introduced, it will be a new, explicit capability with separate validation and backup requirements rather than a flag on the existing reader.

### 2. APA extents are normalized and validated once

PFS uses `(subpart, block/sector)` addressing while APA can place the main partition and its sub-partitions anywhere on disk.

`ApaVolume` converts that logical PFS address space into physical LBAs. APA parsing also rejects main/sub extents that extend outside the backing device before a filesystem layer receives them.

This layout model was hardware-validated on an HDD containing non-contiguous HDL layouts and normal PFS partitions; the stricter extent-bounds check is awaiting the next Chisato hardware pass.

### 3. Byte-range reads are first-class

DriveForge can read arbitrary ranges of a PFS file, including reads starting or ending in the middle of a 512-byte sector. This is required by a Windows filesystem provider, where requests are not guaranteed to align to PS2 sectors or PFS zones.

Aligned portions are currently batched rather than issuing one host call per sector.

### 4. SEGI traversal is hidden behind the reader

A caller does not need to understand the PFS SEGD/SEGI descriptor scheme. The reader walks direct and indirect descriptors and presents one logical file byte stream.

### 5. Host export is not filesystem parsing

Filename conversion, Windows reserved-name handling, case-insensitive collisions, recursive directory export, cycle detection, and host write errors belong to `ps2driveforge_host`, not `ps2driveforge_core`.

GUI and CLI use the same exporter through `DriveSession`; later drag-and-drop/Dokany work can reuse the same policy.

### 6. Cross-layer behavior has a generated-image regression

DriveForge generates a real APA/PFS `.img` during tests, reopens it through production `FileBlockDevice`, traverses APA/PFS including a sub-partition and SEGI-backed file, recursively exports it, and verifies output by SHA-256.

That does not prove compatibility with every real HDD, but it makes multi-layer regressions reproducible without requiring hardware for every commit.

### 7. The performance discussion now has counters

`InstrumentedBlockDevice` records backing read calls/bytes/largest read/failures after PFS batching and APA translation. `--stats` exposes those counters for CLI operations.

This is instrumentation, not a benchmark result.

## Where DriveForge is *not* faster yet

The project must keep this section honest.

Current Windows raw-device I/O is deliberately conservative:

```text
PFS Reader
  -> ApaVolume
    -> InstrumentedBlockDevice
      -> PhysicalDrive
        -> mutex
        -> SetFilePointerEx
        -> synchronous ReadFile
```

The image backend similarly serializes access around one `std::ifstream`.

The PFS reader batches aligned data reads in groups of up to 128 sectors (64 KiB), while host export requests up to 1 MiB at a time. The 1 MiB request can therefore still become multiple lower-level reads.

There is currently:

- no metadata cache;
- no inode cache;
- no directory cache;
- no read-ahead;
- no Windows overlapped I/O;
- no queueing of independent reads;
- no benchmark proving a throughput advantage over pfsshell/pfsfuse.

This is intentional. Correctness, reproducible tests and hardware validation come before optimization.

## Planned performance path

Performance work should improve the data path instead of replacing Dokany or writing a custom kernel driver.

Planned steps:

1. backing-I/O counters — **implemented**;
2. add logical/metadata/payload/timing counters;
3. cache validated PFS metadata and directory results;
4. add a block/window cache below PFS;
5. coalesce adjacent reads across one APA extent;
6. increase sequential read windows based on measured device behavior;
7. add Windows overlapped reads where the backing device benefits;
8. remove unnecessary serialization from immutable/read-only paths;
9. benchmark physical IDE/SATA adapters, USB bridges, and image files separately;
10. compare CLI extraction and mounted-filesystem workloads against current pfsshell/pfsfuse builds.

## Planned Explorer model

pfsshell's documented Windows `pfsfuse` example mounts one selected PFS partition to a drive letter.

DriveForge's target namespace is broader:

```text
PS2HDD (P:)\
  Partitions\
    __system\
    __common\
    +OPL\
  Games\              # synthetic HDL view, later
  System\             # controlled metadata/recovery view, later
```

This is a **design target for the Darkness milestone**, not a feature currently available in Chisato.

## How to use this document during development

Whenever a change is described as making DriveForge "faster" or "better than pfsshell":

1. identify the specific old bottleneck or UX constraint;
2. point to the DriveForge layer that removes or isolates it;
3. add a test for correctness;
4. use `--stats` and add a timed benchmark if the claim is about speed;
5. update this document with measured results rather than assumptions.

That rule is meant to keep the project understandable years after the original implementation decisions are forgotten.
