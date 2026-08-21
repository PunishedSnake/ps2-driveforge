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

### GUI/export validation

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

A real non-empty file was extracted from:

```text
__common:/OPL/conf_hdd.cfg
size: 20 bytes
content: hdd_partition=+OPL\r\n
SHA-256: E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C
```

The supplied artifact was independently re-hashed and matched.

### Remaining real-hardware coverage gap: PFS SEGI

The disk does not contain a suitable large/fragmented **PFS** file; large content is stored as HDL game partitions. Direct real-HDD PFS SEGI remains unobserved. SEGI plus APA main/sub crossing remain protected by deterministic generated-image E2E.

This explicit gap does not block Darkness because the missing case has end-to-end synthetic coverage.

## 0.4.0-dev "Darkness" — standalone/shared Dokany path hardware validated

### Native GUI theme

Observed on Windows 11:

- System/Light/Dark switching changes dynamically while the GUI is running;
- dark title bar, TreeView, ListView/header, and status area render correctly enough for normal use;
- the physical HDD remains browseable while themes change;
- source access semantics are unchanged.

### Initial mount regression

The first `PhysicalDrive3 -> P:` mount registered successfully, but Explorer initially reported:

```text
P:\ is not accessible.
The file exists.
```

Cause: Dokany `ZwCreateFile` passes NT `FILE_*` create dispositions, while the first adapter compared them with Win32 `CreateFileW` constants. `FILE_OPEN == 1` was mistaken for `CREATE_NEW == 1`, returning `STATUS_OBJECT_NAME_COLLISION` for the root.

The corrected policy models NT dispositions explicitly and has portable regression coverage under MSVC and Linux sanitizers.

### Corrected real-HDD Explorer mount — passed

Observed successfully:

- `P:` appeared as PS2 DriveForge / `PS2PFS`;
- Windows reported roughly 149 GiB total size and intentionally 0 bytes free for the read-only view;
- `P:\` opened normally;
- `P:\Partitions` enumerated;
- `P:\Partitions\+OPL` showed `CFG`, `THM`, `LNG`, `ART`, `VMC`, `CHT`, `APPS`;
- `P:\Partitions\__common\OPL` resolved;
- Explorer copied `conf_hdd.cfg` through the mount;
- the mounted copy was 20 bytes with SHA-256 `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`;
- creating `write-test.txt` was rejected;
- Dokany write-protect stopped creation before any source write path;
- callback-level mutation rejection remained active;
- the project still had no `BlockDevice::write()` API;
- `PhysicalDrive` still requested `GENERIC_READ` only;
- unmount completed cleanly and the drive letter disappeared.

Validated end-to-end path:

```text
PhysicalDrive3 / GENERIC_READ
 -> APA v2
 -> PFS
 -> DriveSession
 -> ReadOnlyMountView
 -> DokanyMountController
 -> Explorer
 -> copied host file
 -> matching SHA-256
```

## Final Darkness merge gate: integrated GUI workflow

After the successful standalone/shared-controller mount, Darkness added the final Windows workflow:

- SetupAPI enumeration of actual `GUID_DEVINTERFACE_DISK` devices instead of a fixed visible `PhysicalDrive0..31` list;
- `IOCTL_STORAGE_GET_DEVICE_NUMBER` mapping to the real raw-disk number;
- normal DriveForge read-only APA classification;
- controlled UAC relaunch with cancellation fallback;
- automatic startup discovery and one-candidate auto-open;
- direct GUI `Mount read-only`, `Open mounted volume in Explorer`, and `Unmount` using the same `DokanyMountController` as the CLI;
- automatic free-drive-letter selection preferring `P:`.

This exact integrated path is the only remaining hardware blocker before PR #5 can merge:

1. launch `PS2-DriveForge.exe` as a normal user;
2. confirm one UAC relaunch and no loop;
3. confirm startup SetupAPI discovery finds the PS2 HDD with no old fixed list;
4. confirm the sole candidate auto-opens with approximately 149.05 GiB / APA v2 / 43 main partitions;
5. mount from the GUI and confirm a free drive letter is selected automatically;
6. open the mount from the GUI and browse known PFS paths;
7. confirm write/create/rename/delete remain rejected;
8. unmount from the GUI and confirm clean drive-letter removal;
9. rescan and confirm an already-open source is not silently replaced;
10. once cancel UAC and confirm image-capable limited mode remains usable, with manual restart-as-admin recovery;
11. if practical, occupy `P:` and verify fallback to another free letter.

Passing this list closes 0.4 hardware validation.

## Emilia baseline handoff

Preserve the final successful Darkness workload:

```text
cold GUI start
 -> discovery/open
 -> mount
 -> root
 -> Partitions
 -> +OPL
 -> __common\OPL
 -> read/copy conf_hdd.cfg
 -> unmount
```

Explorer issues many repeated metadata/open/enumeration callbacks. This workload plus existing backing-I/O counters and the 215-read Chisato browse baseline is the reference for 0.5 Emilia.

## Update rule

When hardware validates or disproves an APA/PFS, discovery/elevation, GUI/Dokany, extraction/copy, mount-lifecycle, or performance assumption, update this file and add a deterministic regression where possible. Hardware and synthetic evidence must remain explicitly distinguished.
