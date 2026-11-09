# exFAT / BDM library (APA-Jail exFAT side)

APA-Jail drives (PSBBN Definitive Project) carry three MBR entries next to the
APA region: the APA region itself (type 0x17), a 32 MiB recovery partition
(also 0x17, ext2) and an exFAT partition (type 0x07, label `OPL`). OPL reads the
exFAT partition through its BDM driver exactly like a USB stick or MX4SIO card,
so DriveForge treats all three the same way.

## Why DriveForge does not implement exFAT itself

Windows already mounts the exFAT entry as a normal volume, with a journaled,
battle-tested driver. Re-implementing exFAT on the raw disk would add a second
writer racing the OS one. DriveForge therefore:

1. reads the hybrid MBR (`ScanResult::jail`),
2. finds the Windows volume whose disk extent starts at the exFAT entry's
   offset (`find_partition_volume`, `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`),
3. writes through the filesystem with OPL-specific rules.

If Windows has not assigned a letter, assign one in Disk Management, or pick
the folder manually. Inside a disk image the exFAT side must be mounted by the
OS first.

## OPL rules enforced

| Rule | Behaviour |
| --- | --- |
| Layout | `DVD CD ART CFG CHT VMC APPS THM LNG POPS` created on demand |
| Naming | `<ID>.<Title>.iso`, ID canonical (`SLUS_123.45`), 64 chars max, unsafe chars replaced |
| Media | ≤ 750 MiB → `CD/`, else `DVD/`; override with `--cd`/`--dvd` |
| Duplicates | an existing image for the same ID is refused unless overwrite is set |
| Contiguity | file preallocated to full size before copy, written as `.partial`, renamed on success |
| Fragments | extents counted after install and on scan; > 64 is flagged (OPL's BDM fragment tolerance) |
| Assets | ART/CFG/CHT only added, never replaced |

## Commands

```
ps2-driveforge-hdl-tools bdm-scan    E:\
ps2-driveforge-hdl-tools bdm-install E:\ game.iso [--cd|--dvd] [--title "Name"]
ps2-driveforge-hdl-tools bdm-sync    E:\ %LOCALAPPDATA%\PS2 DriveForge\OPL Cache [--apply]
```

WinUI: HDL Tools → *exFAT / BDM library* (Detect jail volume, Choose folder,
Install selected ISO here, Scan library, Add missing assets, Open in Explorer).
Win32: *Games → exFAT / BDM library*.

A badly fragmented image can be fixed by running `bdm-install` again from a
copy after freeing space, or by defragmenting the exFAT volume.
