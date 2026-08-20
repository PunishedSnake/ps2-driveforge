# Real hardware validation

PS2 DriveForge has been validated against a real **149.05 GiB APA v2 PlayStation 2 HDD** through the Windows `PhysicalDrive` backend.

## 0.1.0-dev "Ayanami"

Observed successfully:

- APA v2 MBR detection and full linked-list traversal;
- PFS v3 detection on system and user partitions;
- 8 KiB PFS zone-size validation;
- matching primary and backup PFS superblocks;
- HDL main/sub partition association, including non-contiguous sub-partitions;
- read-only Windows raw-disk access.

The first hardware report included `__net`, `__system`, `__sysconf`, `__common`, `__boot`, `HDLoader Settings`, `+OPL`, and many HDL game partitions.

## 0.2.0-dev "Bocchi"

The same disk was then used to validate the native PFS read path.

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

A second lookup successfully resolved and enumerated the real `CFG` inode:

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

`CFG` was expected to be empty on this disk, so the zero-entry result is a successful validation rather than a missing-data failure.

This validates the real-hardware path through:

```text
Windows PhysicalDrive
        -> APA scan
        -> APA logical volume
        -> PFS superblock
        -> root SEGD inode
        -> data extents
        -> directory entries
        -> child inode resolution
```

Source-device access remained read-only for all tests.

## Next hardware milestone

0.3.0-dev "Chisato" adds a native Windows GUI and recursive host export. The next useful hardware checks are:

1. browse the same `+OPL` tree in `PS2-DriveForge.exe`;
2. recursively export the empty `+OPL` tree and confirm the seven directories are created on the host;
3. later validate file extraction on a non-empty PFS partition.
