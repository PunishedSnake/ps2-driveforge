# PS2 DriveForge architecture

## Goal

Build a Windows-first PS2 HDD management stack that can eventually expose an APA disk to Windows Explorer while keeping parsing, host operations and presentation independent.

The architecture is intentionally different from wrapping pfsshell's interactive device/mount/current-directory model. See [`pfsshell-comparison.md`](pfsshell-comparison.md) for the evidence and the distinction between implemented behavior and future performance targets.

## Current layers

```text
 Native Win32 GUI        CLI        future Dokany provider
        |                 |                 |
        +-------- ps2driveforge_host -------+
                          |
                 ps2driveforge_core
                          |
               +----------+----------+
               |          |          |
              APA        PFS       HDL/MBR
               |          |          |
               +----------+----------+
                          |
                     BlockDevice
                   /             \
             disk image      PhysicalDriveN
```

### `BlockDevice`

`BlockDevice` is deliberately byte-addressed. Format code decides sector size and alignment. This avoids leaking Windows handles or PS2SDK/iomanX semantics into filesystem code.

Current source backends are read-only:

- `FileBlockDevice` for images;
- `PhysicalDrive` for `\\.\PhysicalDriveN` on Windows.

The absence of `write()` is a safety boundary. Future mutation should introduce an explicit writable capability rather than quietly widening this interface.

### APA

APA owns physical partition-table interpretation:

- 1024-byte header parsing/checksum;
- linked-list traversal;
- partition diagnostics;
- main/sub-partition metadata.

It does not know PFS directory/file semantics.

### `ApaVolume`

`ApaVolume` is the translation boundary between PFS logical sub-partition addressing and physical APA extents:

```text
PFS subpart 0 -> APA main extent
PFS subpart 1 -> APA sub extent 0
PFS subpart 2 -> APA sub extent 1
...
```

Only this layer adds a selected extent's physical start LBA. This prevents physical-layout assumptions from leaking upward.

### PFS

The PFS reader owns on-disk filesystem semantics:

```text
superblock
  -> SEGD inode metadata
  -> optional SEGI chain
  -> logical byte stream
  -> directory entries
  -> path resolution
```

It does **not** own Windows filename policy or host file creation.

### `ps2driveforge_host`

The host layer intentionally knows about the destination filesystem. Current responsibilities include:

- recursive export;
- host path construction;
- Windows-invalid character conversion;
- reserved DOS device names;
- case-insensitive collision handling;
- cycle/depth protection;
- cleanup of partial files after failure.

This separation is important for future GUI drag/drop and Dokany work: host policy can be shared without contaminating on-disk parsing.

## Release-train implementation map

### 0.1 Ayanami: APA read-only core

Implemented and hardware validated:

- file-backed disk images;
- read-only `\\.\PhysicalDriveN` backend on Windows;
- exact 1024-byte APA header layout;
- APA checksum validation;
- Sony MBR signature validation;
- linked-list traversal with cycle/out-of-range detection;
- `next`/`prev` consistency diagnostics;
- main/sub partition metadata and logical size accounting;
- APA extent translator (`main + sub` addressing);
- PFS primary/backup superblock probe and validation.

### 0.2 Bocchi: PFS read path

Implemented and hardware validated:

```text
ApaVolume
  -> PFS SuperBlock
  -> root SEGD inode
  -> direct data extents
  -> optional SEGI chains
  -> byte-range reader
  -> directory iterator
  -> path resolver
```

The reader validates inode checksum/magic, sub-part references, address overflow, descriptor ranges, directory-entry boundaries and SEGI metadata.

### 0.3 Chisato: Windows browser and host export

Current native frontend uses Win32/Common Controls:

```text
Main window
  +-- TreeView      APA main partitions
  +-- ListView      PFS directory contents / metadata
  +-- Status bar    current path + READ ONLY state
```

The GUI owns no filesystem parser logic. Selecting a PFS partition constructs `ApaVolume` + `pfs::Reader`; navigation uses `resolve()` and `list_directory()`.

Host export is reusable:

```text
PFS path
  -> resolve inode
  -> recurse directories
  -> sanitize host filename
  -> stream regular files
  -> host filesystem
```

## Dependency rules

These are architecture invariants, not suggestions:

1. `ps2driveforge_core` must not include Win32 GUI/Dokany/host-path policy.
2. PFS must access physical data through `ApaVolume`, never by adding APA LBAs itself.
3. Frontends must not reimplement APA/PFS parsing.
4. Windows filename conversion must not alter PFS-visible names.
5. Read-only parsing must remain usable without loading GUI code.
6. Performance caches belong at explicit layers and must not weaken validation.
7. Future write support must be a separate capability with backup/recovery semantics.

If a new feature appears to require violating one of these, update the architecture deliberately rather than creating an accidental dependency.

## Important address units

When debugging PFS, write down the unit at each step. The code handles several incompatible address spaces:

```text
host bytes
  <-> 512-byte PS2 sectors
  <-> 1024-byte PFS metadata blocks
  <-> PFS zones
  <-> APA logical extents
  <-> physical disk LBA
```

Common mistakes:

- interpreting `BlockInfo.number` as a sector for file data;
- using payload zone arithmetic for inode metadata;
- pre-adding an APA extent start before calling `ApaVolume`;
- assuming APA sub-partitions are physically contiguous.

See [`apa-format-notes.md`](apa-format-notes.md) and [`pfs-format-notes.md`](pfs-format-notes.md).

## Current concurrency/performance reality

The API is designed so the future Dokany provider does not need a global shell mount/current directory, but the backing I/O is not yet highly concurrent.

Today:

```text
PhysicalDrive::read
  -> per-device mutex
  -> SetFilePointerEx
  -> synchronous ReadFile
```

and the image backend similarly serializes one `ifstream` seek/read state.

This is correct for Chisato and known to be a performance limit. See [`performance.md`](performance.md) before changing read batching, caching or Windows I/O primitives.

## Explorer integration

Dokany belongs above `ps2driveforge_core`/`ps2driveforge_host`, not inside them.

Proposed Darkness namespace:

```text
P:\
  Partitions\
    __system\
    __common\
    +OPL\
  Games\                # synthetic HDL view, later
  System\
    MBR.bin              # controlled metadata/recovery view, later
```

The GUI may present a friendlier flat view while the filesystem provider uses explicit namespaces to avoid collisions between partition names and synthetic folders.

This namespace is a design target, not a Chisato feature.

## Performance direction

Do not optimize by writing a custom kernel filesystem/storage driver first. Improve the user-mode path and measure it:

- instrumentation counters;
- immutable metadata/inode cache;
- directory cache;
- block/read-window cache;
- sequential read-ahead;
- larger/adaptive aligned windows;
- coalescing within APA extents;
- offset/overlapped Windows reads;
- parallel read requests where the device benefits.

Detailed benchmark rules live in [`performance.md`](performance.md).

## Safety invariants before write support

Before a physical-disk write path exists, the project requires:

1. parser fuzz/unit tests;
2. metadata backup format;
3. read-only-by-default physical device access;
4. explicit validation of every target extent;
5. journaling/transaction or other interruption-recovery strategy for APA/PFS metadata writes;
6. destructive testing against disposable images before real HDDs;
7. explicit user opt-in to writable mode.

The current source-device path remains read-only end to end.

## Debugging starting points

When a future hardware test fails:

| Symptom | First layer to inspect |
| --- | --- |
| APA not detected / chain stops | `apa.cpp` + `apa-format-notes.md` |
| PFS superblock invalid | `pfs::probe()` |
| inode checksum/magic failure | PFS metadata addressing |
| wrong data but valid inode | zone -> sector arithmetic / SEGD-SEGI traversal |
| only one sub-partition fails | `ApaVolume` logical extent translation |
| directory garbage | 512-byte dentry boundary parsing |
| export path/name failure | `ps2driveforge_host`, not PFS core |
| slow sequential reads | `performance.md`, backing-read counters/batching |
| GUI-only issue | Win32 frontend; reproduce through CLI/core first |

The goal is to identify which abstraction is wrong before adding compatibility hacks to the layer above it.
