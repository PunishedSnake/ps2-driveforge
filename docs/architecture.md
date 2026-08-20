# PS2 DriveForge architecture

## Goal

Build a Windows-first PS2 HDD management stack that can eventually expose an APA disk to Windows Explorer while keeping parsing, host operations and presentation independent.

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

`BlockDevice` is deliberately byte-addressed. Format code decides its own sector size and alignment. This avoids leaking Windows handles or PS2SDK's `iomanX` model into the filesystem implementation.

`ps2driveforge_core` does not create host files. It parses APA/PFS and exposes read operations.

`ps2driveforge_host` contains operations that intentionally touch the host filesystem, currently recursive PFS export and filename conversion. Keeping this separate prevents a later Dokany/GUI implementation from duplicating transfer logic or contaminating the on-disk parser with Windows presentation rules.

## 0.1 Ayanami: APA read-only core

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

No write API exists. That remains intentional.

## 0.2 Bocchi: PFS read path

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

PFS sees one logical volume even when APA expands it across sub-partitions. `ApaVolume` owns this mapping so GUI and future Dokany code do not need to understand physical APA extents.

The reader validates inode checksum/magic, 512-byte dentry boundaries and range limits. It handles unaligned reads and indirect SEGI descriptor chains instead of assuming files are contiguous.

## 0.3 Chisato: Windows browser and host export

The first native Windows frontend uses only Win32/Common Controls:

```text
Main window
  +-- TreeView      APA main partitions
  +-- ListView      PFS directory contents / metadata
  +-- Status bar    current path + READ ONLY state
```

The GUI owns no filesystem parser logic. Selecting a PFS partition constructs `ApaVolume` + `pfs::Reader`; folder navigation uses `resolve()` and `list_directory()`; single-file extraction uses the same read API as the CLI.

Host export is a reusable layer above PFS:

```text
PFS path
  -> resolve inode
  -> recurse directories
  -> sanitize host filename
  -> stream regular files (1 MiB chunks)
  -> host filesystem
```

Safety features include directory-depth limits, active-inode cycle detection, Windows reserved-name handling and case-insensitive collision avoidance.

## Explorer integration

Dokany belongs above `ps2driveforge_core`/`ps2driveforge_host`, not inside them. Its callbacks are multithreaded, so public read operations in the core must either be immutable or synchronize device/cache access.

Proposed namespace:

```text
P:\\
  Partitions\\
    __system\\
    +OPL\\
    +BOOT\\
  Games\\                # virtual HDL view, later
  System\\
    MBR.bin               # controlled virtual metadata view, later
```

The GUI can present a friendlier flat view while the Dokany provider uses explicit namespaces to avoid collisions between partition names and synthetic folders.

## Performance direction

Do not optimize by writing a kernel driver. Optimize the user-mode data path:

- overlapped Windows reads;
- aligned large read windows;
- metadata/inode cache;
- directory cache;
- sequential read-ahead;
- coalesced writes only after the write path is proven safe;
- parallel read requests where the backing device benefits.

## Safety invariants

Before write support exists:

1. parser fuzz/unit tests;
2. metadata backup format;
3. read-only-by-default physical device access;
4. explicit validation of every target extent;
5. journaling/transaction strategy for APA/PFS metadata writes.

The current source-device path remains read-only end to end.
