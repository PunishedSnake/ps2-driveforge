# Real hardware validation

PS2 DriveForge has been validated against a real **149.05 GiB APA v2 PlayStation 2 HDD** through the Windows read-only `PhysicalDrive` backend.

This file records physical-hardware observations separately from synthetic/image coverage. A feature is not called hardware-validated merely because a unit test likes it. Disks have a habit of introducing variables that test doubles politely omit.

All host source-device operations recorded below were read-only.

## Current Frieren status

The historical sections below validate the read/discovery/export/mount foundation inherited by Frieren.

Frieren image-only HDL/PFS mutation and recovery/rescue logic is covered by deterministic writable-image tests, but **host physical writes are not yet hardware-validated and remain unavailable**.

Recovery parity also requires real artifact interchange with FHDB Manager:

```text
FHDB Manager creates HDDRESCUE/HDDMBR/HDDRAW/HDDMETA
 -> DriveForge validates and consumes representative samples

DriveForge creates the shared artifacts
 -> FHDB Manager validates and consumes representative samples
```

Those round trips must be recorded before the interoperability gate is considered complete. The fact that both implementations agree in synthetic tests is necessary, not magical proof that every bridge, cache and real disk has signed the same contract.

## Test disk characteristics

Observed regression target:

- APA v2;
- 190 APA headers;
- 43 main partitions in the GUI;
- PFS v3 system/user partitions;
- 8 KiB PFS zones on observed PFS volumes;
- matching primary/backup PFS superblocks;
- many HDL game partitions and APA subpartitions;
- non-contiguous APA layouts;
- `+OPL` PFS partition for directory/export tests;
- `__common:/OPL/conf_hdd.cfg` as a stable real regular-file payload.

The disk includes `__net`, `__system`, `__sysconf`, `__common`, `__boot`, `HDLoader Settings`, `+OPL` and many HDL game partitions.

## 0.1.0-dev "Ayanami"

Hardware-validated:

```text
Windows PhysicalDrive
 -> raw read-only byte access
 -> APA MBR/header parser
 -> linked-list traversal
 -> partition/subpartition model
 -> PFS superblock probe
```

Observed successfully: APA v2 MBR detection, full linked-list traversal, PFS v3 detection, 8 KiB zones, matching PFS backups, HDL main/sub association, non-contiguous layout handling and `GENERIC_READ`-only physical access.

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

`+OPL:/CFG` was expected to be empty and correctly returned `0 entries`. This provides real-hardware evidence for observed root/child inode addressing, SEGD checksum/magic validation, directory payload reads, dentry parsing, child inode resolution and explicit path traversal.

## 0.3.0-dev "Chisato" - hardware validated

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

The full scan completed without fatal APA diagnostics. Observed PFS partitions reported PFS v3, 8 KiB zones and matching primary/backup superblocks. The GUI showed 43 main partitions.

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

The native GUI opened the disk read-only, displayed the partition tree, browsed `+OPL`, retained explicit `READ ONLY` status and showed cumulative backing reads.

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

The supplied artifact was independently rehashed and matched.

### Remaining real-hardware coverage gap: PFS SEGI

The disk does not contain a suitable large/fragmented **PFS** file; large content is stored as HDL game partitions. Direct real-HDD PFS SEGI remains unobserved. SEGI plus APA main/sub crossing remain protected by deterministic generated-image E2E.

This explicit gap did not block the historical read-only release because the missing case had end-to-end synthetic coverage. It should not be silently promoted into physical-write evidence for Frieren.

## 0.4.0-dev "Darkness" - hardware validated

### Native GUI theme

Observed on Windows 11:

- System/Light/Dark switching changes dynamically while the GUI is running;
- dark title bar, TreeView, ListView/header and status area render correctly enough for normal use;
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

### Corrected real-HDD Explorer mount - passed

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
- the project still had no read-side `BlockDevice::write()` API;
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

### Integrated GUI workflow - passed

The final Windows 11 Darkness pass validated:

- normal-user launch performs one controlled UAC relaunch without a loop;
- SetupAPI enumerates real disk interfaces rather than a fixed guessed range;
- the APA probe identifies the sole PS2 HDD candidate and the GUI opens it automatically;
- the device reports the expected approximately 149.05 GiB, APA v2 and 43 main partitions;
- read-only mount selects a free drive letter;
- Explorer opens the shared `Partitions` namespace;
- known `+OPL` and `__common\OPL` paths remain browseable;
- create/write operations remain rejected;
- unmount removes the drive cleanly;
- rescan does not silently replace an already-open source;
- cancelling elevation leaves image-capable limited mode usable.

This closed the historical 0.4 read/mount hardware gate. The GUI and diagnostic mount frontend use the same `DokanyMountController`; no second filesystem implementation is involved.

## Emilia baseline handoff

Preserved successful read workload:

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

Explorer issues repeated metadata/open/enumeration callbacks. This workload plus the backing-I/O counters and Chisato baseline remains the 0.5 Emilia reference.

## Frieren hardware validation to add

Before Frieren can claim equivalent real-device confidence for its new functionality, preserve exact evidence for:

1. grouped HDL main/sub presentation against the real 190-header disk;
2. image-created HDL game boot/read behavior after writing the image back through an independently controlled method;
3. OPL ART/CFG/CHT/TAR assets written by DriveForge and consumed by OPL;
4. delete result and clean APA scan after console use;
5. FHDB Manager-created `HDDRESCUE`, `HDDMBR`, `HDDRAW`, `HDDMETA` and `FORENSIC.TXT` consumed by DriveForge;
6. DriveForge-created shared artifacts consumed by FHDB Manager;
7. full bootstrap rescue restore on a disposable image followed by an independent PS2-side validation path;
8. future physical host writes only after the separate physical-write gate is satisfied.

Do not convert an image test into a real-HDD claim by copying the word "physical" into the heading. The disk deserves slightly more evidence than that.

## Update rule

When hardware validates or disproves an APA/PFS/HDL, recovery, discovery/elevation, GUI/Dokany, extraction/copy, mount-lifecycle or performance assumption, update this file and add a deterministic regression where possible.

Hardware, image and synthetic evidence remain explicitly distinguished. For shared recovery formats, record both artifact producer and consumer platform.
