# Real hardware validation

PS2 DriveForge has been validated against a real **149.05 GiB APA v2 PlayStation 2 HDD** through the Windows `PhysicalDrive` backend.

This file records observations from physical hardware separately from synthetic tests. A feature appearing in unit tests is not described as hardware-validated until it has actually run against a real disk.

All source-device operations described below were read-only.

## Test disk characteristics

Observed layout characteristics that make this disk useful as a regression target:

- APA v2;
- 190 APA headers detected by the Chisato discovery path;
- 43 main partitions visible in the GUI;
- multiple PFS v3 system/user partitions;
- 8 KiB PFS zones on observed PFS partitions;
- matching primary/backup PFS superblocks on observed PFS partitions;
- many HDL game partitions and APA sub-partitions;
- main/sub layouts that demonstrate physical extents must not be assumed contiguous;
- `+OPL` PFS partition suitable for directory/export validation;
- `__common` PFS partition containing at least one real regular file suitable for extraction validation.

The report includes `__net`, `__system`, `__sysconf`, `__common`, `__boot`, `HDLoader Settings`, `+OPL`, and many HDL game partitions.

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

`CFG` was expected to be empty and correctly returned `0 entries`.

This gives real-hardware evidence for root/child inode addressing, SEGD checksum/magic validation on the observed nodes, directory payload reads, dentry parsing, child inode resolution, and explicit path traversal.

## 0.3.0-dev "Chisato" — hardware validated

Chisato was validated on the same physical disk after the pre-hardware hardening pass.

The tested build included:

- shared `DriveSession` for CLI and GUI;
- read-only physical-drive discovery;
- backing-I/O counters;
- recursive host export;
- generated-image E2E coverage for main/sub and SEGI;
- APA/PFS corruption regression tests.

### Physical-drive discovery

```powershell
ps2-driveforge-inspect.exe --detect-physical
```

Observed:

```text
PhysicalDrive0    931.51 GiB    not APA
PhysicalDrive1    447.13 GiB    not APA
PhysicalDrive2    1.82 TiB      not APA
PhysicalDrive3    149.05 GiB    PS2 APA    v2    190 headers
PhysicalDrive4    931.51 GiB    not APA
PhysicalDrive5    223.57 GiB    not APA

Openable drives: 6, PS2 APA candidates: 1
```

The discovery pass therefore selected exactly the expected PS2 HDD and did not misidentify the other openable Windows disks. Discovery remained `GENERIC_READ` only.

### Full APA/PFS scan

The complete physical scan succeeded without fatal APA diagnostics.

Observed PFS partitions (`__net`, `__system`, `__sysconf`, `__common`, `__boot`, `HDLoader Settings`, `+OPL`) all reported:

- PFS v3;
- 8 KiB zones;
- filesystem sub count 0 for these observed volumes;
- matching primary/backup superblocks.

The GUI presented **43 main partitions** and reported diagnostics as clean.

### Instrumented `+OPL` browse baseline

```powershell
ps2-driveforge-inspect.exe --stats --physical 3 --browse +OPL
```

The expected seven directories were returned again. The first recorded physical browse baseline is:

```text
APA scans:             1
PFS browse operations: 1
PFS export operations: 0
Backing read calls:    215
Backing bytes read:    206.50 KiB
Average backing read:  983 B
Largest backing read:  1.00 KiB
Failed backing reads:  0
```

This is a correctness/performance baseline for later Emilia work, **not** a throughput comparison against pfsshell/pfsfuse.

### Native Win32 GUI

The Chisato GUI successfully:

- detected `PhysicalDrive3` as the only PS2 APA candidate;
- opened the disk read-only;
- displayed the full partition tree;
- showed 43 main partitions;
- browsed `+OPL` and its child directories;
- retained explicit `READ ONLY` state;
- displayed cumulative backing-read counters;
- completed the planned GUI/navigation checks without crashes or filesystem errors.

One presentation-only issue was found: UTF-8 punctuation in wide Win32 string literals was compiled with the wrong MSVC source character set and appeared as mojibake (`â€“`/`â€”`). This is not an APA/PFS data-decoding failure. The GUI target now explicitly builds with MSVC `/utf-8`.

### Recursive `+OPL` export

Recursive export of the real `+OPL` tree completed successfully and produced the expected host directories:

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

This validates real-HDD traversal plus recursive host directory creation. The observed `+OPL` directories were empty, so this case alone does not validate file payload bytes.

### Real regular-file export

A non-empty PFS path under `__common` was successfully browsed/exported. The extracted file was:

```text
__common:/OPL/conf_hdd.cfg
size: 20 bytes
content: hdd_partition=+OPL\r\n
SHA-256: E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C
```

The SHA-256 was independently recomputed from the supplied extracted artifact and matches the recorded PowerShell hash. This confirms the extracted artifact is stable and exactly 20 bytes. It is not presented as a cross-tool source comparison because an independent pfsshell extraction/hash was not recorded for this file.

### Remaining real-hardware coverage gap: SEGI / large fragmented PFS

A sufficiently large or fragmented **PFS** file was not available on this disk. Large content on the disk is stored as HDL game partitions, which exercises a different future DriveForge subsystem and cannot be used as a PFS SEGI test.

Therefore:

- direct real-HDD PFS browsing/export is hardware validated;
- regular-file payload extraction is hardware validated;
- real PFS SEGI traversal remains **not hardware validated**;
- SEGI, main-to-sub extent crossing, and large fragmented-file behavior remain protected by the generated sparse-image E2E regression test until suitable physical PFS data becomes available.

This gap does **not** block Chisato 0.3 because Chisato's user-facing scope is read-only browsing/export, and the missing case has deterministic end-to-end synthetic coverage. It should remain listed as a coverage gap rather than being silently treated as proven.

## Chisato validation conclusion

For the tested physical HDD, Chisato validates the complete intended 0.3 path:

```text
Windows physical discovery
        -> PhysicalDrive GENERIC_READ
        -> APA v2 scan
        -> DriveSession
        -> PFS v3 probe
        -> directory/path resolution
        -> native GUI browse
        -> recursive host export
        -> real regular-file extraction
```

No source write path was enabled or used.

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
