# Real hardware validation

PS2 DriveForge has been validated against a real **149.05 GiB APA v2 PlayStation 2 HDD** through the Windows `PhysicalDrive` backend.

This file records observations from physical hardware separately from synthetic tests. A feature appearing in unit tests is not described as hardware-validated until it has actually run against a real disk.

All source-device operations described below were read-only.

## Test disk characteristics

Observed layout characteristics that make this disk useful as a regression target:

- APA v2;
- multiple PFS v3 system/user partitions;
- 8 KiB PFS zones on observed PFS partitions;
- matching primary/backup PFS superblocks on observed PFS partitions;
- many HDL game partitions;
- main/sub-partition layouts that demonstrate physical extents must not be assumed contiguous;
- `+OPL` PFS partition suitable for directory-reader validation.

The report included `__net`, `__system`, `__sysconf`, `__common`, `__boot`, `HDLoader Settings`, `+OPL`, and many HDL game partitions.

## 0.1.0-dev "Ayanami"

Validated path:

```text
Windows PhysicalDrive
        -> raw byte reads
        -> APA MBR/header parser
        -> linked-list traversal
        -> partition/sub-partition model
        -> PFS superblock probe
```

Observed successfully:

- APA v2 MBR detection;
- full linked-list traversal;
- PFS v3 detection on system/user partitions;
- 8 KiB PFS zone-size validation;
- matching primary and backup PFS superblocks;
- HDL main/sub partition association;
- non-contiguous physical layouts handled without assuming adjacency;
- no write access requested by the Windows backend.

## 0.2.0-dev "Bocchi"

The same disk was used to validate the native PFS read path.

Command:

```powershell
ps2-driveforge-inspect.exe --physical 3 --browse +OPL
```

Observed PFS root:

```text
PFS browse: +OPL:/
Type    Size          Sub       Inode         Name
------------------------------------------------------------------------------
DIR     512 B         0         532           CFG
DIR     512 B         0         534           THM
DIR     512 B         0         536           LNG
DIR     512 B         0         538           ART
DIR     512 B         0         540           VMC
DIR     512 B         0         542           CHT
DIR     512 B         0         544           APPS
7 entries
```

A second lookup resolved and enumerated the real `CFG` inode:

```powershell
ps2-driveforge-inspect.exe --physical 3 --browse +OPL CFG
```

Result:

```text
PFS browse: +OPL:/CFG
Type    Size          Sub       Inode         Name
------------------------------------------------------------------------------
0 entries
```

`CFG` was expected to be empty on this disk. The zero-entry result is therefore a successful directory-resolution/enumeration result, not evidence of missing data.

Validated path:

```text
Windows PhysicalDrive
        -> APA scan
        -> ApaVolume logical extent translation
        -> PFS superblock
        -> root SEGD inode
        -> payload extents
        -> directory-entry parsing
        -> child inode resolution
        -> nested directory enumeration
```

This specifically gives us real-hardware evidence for:

- root inode metadata addressing;
- SEGD checksum/magic validation on the observed root/children;
- directory payload reads;
- 512-byte dentry parsing on the observed directory data;
- exact child inode `(subpart, number)` resolution;
- path traversal without shell-global current-directory state.

It does **not** by itself prove every possible SEGI/fragmentation/file-content case. Those currently rely on synthetic regression tests until a suitable real PFS file exercises them.

## 0.3.0-dev "Chisato"

Chisato adds the native Windows GUI and recursive host export while keeping the Ayanami/Bocchi source reader underneath.

Next physical-HDD checks:

1. open the same disk through `PS2-DriveForge.exe`;
2. select `+OPL` and confirm the seven root directories appear in the GUI;
3. recursively export the empty `+OPL` tree and confirm seven host directories are created;
4. find a non-empty PFS tree and compare exported file hashes/content against an independent known-good path where possible;
5. exercise a sufficiently large/fragmented PFS file to add real-hardware evidence for SEGI traversal.

Suggested empty-tree command:

```powershell
ps2-driveforge-inspect.exe --physical 3 --extract +OPL / exported-OPL
```

Expected host structure at the time of this validation:

```text
exported-OPL\
  CFG\
  THM\
  LNG\
  ART\
  VMC\
  CHT\
  APPS\
```

## When to update this document

Update this file whenever a real disk validates or disproves an assumption about:

- APA chain/layout behavior;
- PFS version/zone behavior;
- SEGD/SEGI traversal;
- directory parsing;
- sub-partition addressing;
- GUI/Dokany visibility;
- extraction correctness;
- future write/recovery behavior.

If a hardware failure changes format interpretation, add a synthetic regression test and update the relevant format note in the same change.
