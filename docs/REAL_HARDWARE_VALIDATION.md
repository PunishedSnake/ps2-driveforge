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

## 0.3.0-dev "Chisato" — pre-hardware status

Chisato has completed its pre-hardware hardening pass in CI, but the changes below are **not yet claimed as real-HDD validated**.

Synthetic/CI evidence now covers:

- a generated sparse APA/PFS `.img` reopened through production `FileBlockDevice`;
- recursive export verified by SHA-256;
- data crossing APA main/sub-partition extents;
- generated SEGI-backed file export;
- nested and empty directories;
- Windows filename/collision handling;
- malformed APA/PFS regression cases;
- `DriveSession` scan/browse/export orchestration;
- backing-I/O instrumentation;
- Windows/MSVC and Clang ASan+UBSan builds/tests.

The next physical-HDD pass should validate the frontend/session/discovery path in a fixed order so failures are easy to isolate.

### Step 1 — read-only physical-drive discovery

Run from an elevated terminal if required:

```powershell
ps2-driveforge-inspect.exe --detect-physical
```

Expected for the previously used disk: one entry should identify the correct `PhysicalDriveN` as a PS2 APA disk, report approximately 149.05 GiB, APA v2, and a non-zero header count.

Record the complete output. If the disk is not detected here, do not continue to higher layers until discovery/Windows visibility is understood.

### Step 2 — instrumented CLI browse

Using the detected index:

```powershell
ps2-driveforge-inspect.exe --stats --physical N --browse +OPL
ps2-driveforge-inspect.exe --stats --physical N --browse +OPL CFG
```

Expected directory results remain:

```text
+OPL:/
  CFG
  THM
  LNG
  ART
  VMC
  CHT
  APPS

+OPL:/CFG
  0 entries
```

Preserve the `I/O statistics` block from both commands. These measurements establish the first physical baseline for later Emilia work; they are not yet a pfsshell performance comparison.

### Step 3 — GUI discovery and browse

Launch `PS2-DriveForge.exe`.

The GUI should now start without forcing an image dialog. Then:

1. choose `File -> Detect PS2 HDDs...`;
2. confirm it lists the same physical disk/index as the CLI;
3. choose `File -> Open physical drive -> PhysicalDriveN`;
4. select `+OPL`;
5. confirm the seven root directories appear;
6. enter `CFG` and confirm it is empty;
7. confirm the status bar remains explicitly `READ ONLY` and displays cumulative backing reads/bytes.

This checks that the GUI and CLI are genuinely sharing `DriveSession` behavior rather than only compiling against it.

### Step 4 — recursive empty-tree export

Run:

```powershell
ps2-driveforge-inspect.exe --stats --physical N --extract +OPL / exported-OPL
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

Because those directories were empty at the last check, this validates recursive directory creation and real-HDD traversal but **not file-content correctness**.

### Step 5 — non-empty PFS file integrity

Find a known non-empty PFS partition/tree and export one or more files. Where an independent known-good extraction route exists, compare file size and SHA-256.

This is the first physical check that can validate actual host file-content correctness rather than only metadata/directory traversal.

### Step 6 — real fragmented/SEGI file when available

If a sufficiently large or fragmented PFS file is available, preserve its export hash/size and the `--stats` output. A successful case will add real-hardware evidence for SEGI traversal, which currently remains synthetic-only.

## Failure isolation during Chisato validation

Use the earliest failed layer rather than debugging everything at once:

| Failure | First suspect |
| --- | --- |
| `--detect-physical` misses disk | Windows visibility / `PhysicalDrive` open / APA MBR probe |
| discovery works, full scan fails | APA chain or new extent-bounds validation |
| CLI browse fails | `DriveSession` -> ApaVolume/PFS reader |
| CLI works, GUI fails | Win32 presentation/session input state |
| browse works, empty export fails | host exporter/path policy |
| metadata works, extracted bytes differ | PFS extent/SEGI/byte-range read path |
| only sub-partition-backed file fails | `ApaVolume` logical extent translation |

## When to update this document

Update this file whenever a real disk validates or disproves an assumption about:

- APA chain/layout behavior;
- PFS version/zone behavior;
- SEGD/SEGI traversal;
- directory parsing;
- sub-partition addressing;
- GUI/Dokany visibility;
- extraction correctness;
- physical-drive discovery;
- backing-I/O baseline behavior;
- future write/recovery behavior.

If a hardware failure changes format interpretation, add a synthetic regression test and update the relevant format/testing note in the same change.
