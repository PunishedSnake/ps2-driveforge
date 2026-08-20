# PS2 DriveForge architecture

## Goal

Build a Windows-first PS2 HDD management stack that can eventually expose an APA disk to Windows Explorer while keeping parsing, mutation and presentation independent.

## Layers

```text
Windows GUI / CLI / Dokany provider
              |
        ps2driveforge_core API
              |
   +----------+----------+
   |          |          |
  APA        PFS       HDL/MBR       (PFS/HDL are later milestones)
   |          |          |
   +----------+----------+
              |
         BlockDevice
       /             \
 disk image      PhysicalDriveN
```

`BlockDevice` is deliberately byte-addressed. Format code decides its own sector size and alignment. This avoids leaking Windows handles or PS2SDK's `iomanX` model into the filesystem implementation.

## Phase 0.1: read-only APA

Implemented now:

- file-backed disk images;
- read-only `\\.\PhysicalDriveN` backend on Windows;
- exact 1024-byte APA header layout;
- APA checksum validation;
- Sony MBR signature validation;
- linked-list traversal with cycle/out-of-range detection;
- `next`/`prev` consistency diagnostics;
- partition and sub-partition metadata;
- CLI inspector;
- synthetic parser tests;
- APA extent translator (`main + sub` addressing);
- PFS primary/backup superblock probe and validation.

No write API exists in 0.1. That is intentional: malformed parsing must not be able to mutate a disk.

## Next: 0.2 PFS read path

The PFS superblock probe and APA extent translator now exist. The next core interfaces should be:

```text
ApaVolume
  -> extents (main + sub partitions)
  -> SectorTranslator
  -> PfsVolume
       -> inode reader
       -> directory iterator
       -> file reader
```

PFS must see one logical volume even when APA expands it across sub-partitions. `SectorTranslator` should own this mapping so neither GUI nor Dokany need to understand APA extents.

## Explorer integration

Dokany belongs above `ps2driveforge_core`, not inside it. Its callbacks are multithreaded, so public read operations in the core must either be immutable or synchronize device/cache access.

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

The GUI can optionally present the friendlier flat view while the Dokany provider uses explicit namespaces to avoid collisions between partition names and synthetic folders.

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
