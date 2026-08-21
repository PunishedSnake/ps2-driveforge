# Real hardware validation

PS2 DriveForge has been validated against a real **149.05 GiB APA v2 PlayStation 2 HDD** through the Windows `PhysicalDrive` backend.

This file records physical-hardware observations separately from synthetic coverage. A feature is not called hardware-validated merely because a unit/generated-image test covers it.

All source-device operations described here were read-only.

## Test disk characteristics

Observed regression target:

- APA v2;
- 190 APA headers;
- 43 main partitions in the GUI;
- PFS v3 system/user partitions;
- 8 KiB PFS zones on observed PFS volumes;
- matching primary/backup PFS superblocks;
- many HDL game partitions and APA sub-partitions;
- non-contiguous APA layouts;
- `+OPL` PFS partition for directory/export tests;
- `__common:/OPL/conf_hdd.cfg` as a stable real regular-file payload.

The disk includes `__net`, `__system`, `__sysconf`, `__common`, `__boot`, `HDLoader Settings`, `+OPL`, and many HDL game partitions.

## 0.1.0-dev "Ayanami"

Hardware-validated:

```text
Windows PhysicalDrive
  -> raw read-only byte access
  -> APA MBR/header parser
  -> linked-list traversal
  -> partition/sub-partition model
  -> PFS superblock probe
```

Observed successfully: APA v2 MBR detection, full linked-list traversal, PFS v3 detection, 8 KiB zones, matching PFS backups, HDL main/sub association, non-contiguous layout handling, and `GENERIC_READ`-only physical access.

## 0.2.0-dev "Bocchi"

The same disk validated the native PFS read path.

```powershell
ps2-driveforge-inspect.exe --physical 3 --browse +OPL
```

Observed root:

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

`+OPL:/CFG` was expected to be empty and correctly returned `0 entries`. This gives real-hardware evidence for observed root/child inode addressing, SEGD checksum/magic validation, directory payload reads, dentry parsing, child inode resolution, and explicit path traversal.

## 0.3.0-dev "Chisato" — hardware validated

Chisato validated the shared `DriveSession`, GUI browser, physical discovery, recursive exporter, generated-image hardening, and backing-I/O instrumentation.

### Physical discovery and scan

The original Chisato discovery pass observed:

```text
PhysicalDrive0    931.51 GiB    not APA
PhysicalDrive1    447.13 GiB    not APA
PhysicalDrive2    1.82 TiB      not APA
PhysicalDrive3    149.05 GiB    PS2 APA    v2    190 headers
PhysicalDrive4    931.51 GiB    not APA
PhysicalDrive5    223.57 GiB    not APA

Openable drives: 6, PS2 APA candidates: 1
```

The full scan completed without fatal APA diagnostics. Observed PFS partitions reported PFS v3, 8 KiB zones, and matching primary/backup superblocks. The GUI showed 43 main partitions.

### `+OPL` backing-I/O baseline

```powershell
ps2-driveforge-inspect.exe --stats --physical 3 --browse +OPL
```

Recorded baseline:

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

This is a correctness/performance baseline for later Emilia work, not a benchmark claim against pfsshell/pfsfuse.

### GUI and recursive export

The native GUI opened the disk read-only, displayed the partition tree, browsed `+OPL`, retained explicit `READ ONLY` status, and showed cumulative backing reads.

Recursive `+OPL` export produced:

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

The observed directories were empty, so this case validates traversal/host-directory creation but not payload bytes.

### Real regular-file export

A real non-empty file was extracted from:

```text
__common:/OPL/conf_hdd.cfg
size: 20 bytes
content: hdd_partition=+OPL\r\n
SHA-256: E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C
```

The supplied artifact was independently re-hashed and matched the recorded value.

### Remaining real-hardware coverage gap: PFS SEGI

The disk does not contain a suitable large/fragmented **PFS** file; its large content is stored as HDL game partitions. Therefore direct real-HDD PFS SEGI traversal is still not observed. SEGI plus APA main/sub crossing remain protected by the deterministic generated sparse-image E2E test.

This gap did not block Chisato and does not block Darkness because the missing case has end-to-end synthetic coverage and is explicitly documented rather than treated as hardware-proven.

## 0.4.0-dev "Darkness" — CLI/Dokany path hardware validated

Darkness adds the read-only Dokany Explorer filesystem and Windows 11 host-UI work.

### Native GUI theme

Observed on Windows 11:

- System/Light/Dark switching changes dynamically while the GUI is running;
- dark title bar, TreeView, ListView/header, and status area render correctly enough for normal use;
- the physical HDD remains browseable while themes change;
- source access semantics are unchanged.

### First Dokany attempt and root-open regression

The first `PhysicalDrive3 -> P:` mount successfully registered the drive letter, but Explorer initially reported:

```text
P:\ is not accessible.
The file exists.
```

The cause was a frontend contract bug: Dokany `ZwCreateFile` passes NT kernel `FILE_*` create-disposition values, but the first adapter compared them with Win32 `CreateFileW` constants. `FILE_OPEN == 1` was therefore mistaken for `CREATE_NEW == 1`, returning `STATUS_OBJECT_NAME_COLLISION` for an existing root.

The corrected policy models NT dispositions explicitly and has a portable regression test under both MSVC and Linux sanitizers.

### Corrected real-HDD Explorer mount — passed

After the fix, the same physical HDD mounted read-only as `P:` and the complete CLI-driven Darkness smoke test passed.

Observed successfully:

- `P:` registered and appeared as **PS2 DriveForge (P:)** / `PS2PFS`;
- Windows reported the physical capacity at roughly **149 GiB**;
- the read-only view intentionally reported **0 bytes free**;
- `P:\` opened normally;
- `P:\Partitions` enumerated the exposed PFS namespace;
- `P:\Partitions\+OPL` enumerated `CFG`, `THM`, `LNG`, `ART`, `VMC`, `CHT`, `APPS`;
- `P:\Partitions\__common\OPL` resolved successfully;
- Explorer copied `conf_hdd.cfg` through the mounted filesystem;
- the mounted copy was exactly 20 bytes and matched SHA-256:
  `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`;
- creating `write-test.txt` was rejected;
- `DOKAN_OPTION_WRITE_PROTECT` stopped the create before any writable source path could exist;
- callback-level mutation rejection remains a second barrier;
- the project still has no `BlockDevice::write()` API;
- `PhysicalDrive` still requests `GENERIC_READ` only;
- unmount completed cleanly and the drive letter disappeared.

The observed end-to-end read path is therefore hardware validated:

```text
PhysicalDrive3 / GENERIC_READ
        -> APA v2
        -> PFS
        -> DriveSession
        -> ReadOnlyMountView
        -> DokanyMountController callbacks
        -> Windows Explorer
        -> copied host file
        -> matching SHA-256
```

### Final Darkness hardware gate: integrated GUI workflow

After the successful CLI-driven mount, Darkness was refactored so the native GUI and standalone mount CLI share one `DokanyMountController`. Windows disk discovery was also changed from a fixed visible `PhysicalDrive0..31` probe list to SetupAPI enumeration of actual `GUID_DEVINTERFACE_DISK` devices, mapped to their real physical-drive numbers with `IOCTL_STORAGE_GET_DEVICE_NUMBER` before the normal read-only APA probe.

The remaining pre-merge hardware test is specifically this new integrated UX:

1. launch `PS2-DriveForge.exe` as a normal user;
2. confirm one controlled UAC relaunch and no elevation loop;
3. confirm SetupAPI startup discovery finds the PS2 HDD without the old fixed PhysicalDrive list;
4. with one PS2 candidate, confirm it opens automatically and reports approximately 149.05 GiB / APA v2 / 43 main partitions;
5. use `File -> Mount read-only` and confirm a free drive letter is selected automatically (prefer `P:`);
6. use `Open mounted volume in Explorer` and recheck `Partitions\+OPL` plus `Partitions\__common\OPL`;
7. optionally repeat the known `conf_hdd.cfg` SHA-256 check;
8. confirm write/create/rename/delete remain rejected;
9. use the GUI `Unmount` command and confirm clean removal of the mount;
10. run `Rescan PS2 HDDs` and confirm discovery does not silently replace an already-open source;
11. once, cancel UAC and confirm the limited process remains capable of image-file use, with `Restart as Administrator` available to regain raw-disk access;
12. if practical, occupy `P:` and verify automatic fallback to another free drive letter.

Passing this integrated workflow is the final hardware blocker before Darkness is marked complete and merged.

## Emilia baseline handoff

Before starting 0.5 Emilia, preserve the final Darkness Explorer workload:

```text
cold GUI start
 -> discovery/open
 -> mount
 -> P:\
 -> P:\Partitions
 -> +OPL
 -> __common\OPL
 -> read/copy conf_hdd.cfg
 -> unmount
```

Explorer issues many repeated metadata/open/enumeration callbacks. That observed workload, together with existing backing-I/O counters and the 215-read Chisato browse baseline, is the reference for measuring inode/directory/block caching, read-ahead, request coalescing, and overlapped physical I/O in Emilia.

## When to update this document

Update this file whenever real hardware validates or disproves assumptions about APA/PFS layout, SEGD/SEGI traversal, directory parsing, sub-partition addressing, GUI/Dokany visibility, extraction/copy correctness, device discovery/elevation, mount lifecycle, backing-I/O behavior, or future write/recovery semantics.

If a hardware failure changes interpretation or frontend policy, add a deterministic regression test and update the relevant format/testing/architecture note in the same change.
