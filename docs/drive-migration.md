# Drive migration and backups

DriveForge clones a PS2 HDD by copying **only what the APA chain owns**, at the
same absolute positions, onto a bigger drive, a smaller-but-sufficient drive or
a sparse backup image.

## Why a plain positional copy is a complete migration

* APA partitions are addressed by absolute LBA; moving them would mean
  rewriting every header, sub-partition table and HDL allocation table.
* `mbr.nsector` is always written as 0 by Sony's and PS2SDK's formatters: the
  APA master stores no disk size.
* libapa computes installable space from the device's real `totalLBA`
  (`iop/hdd/libapa/src/free.c`), so space after the last partition is free on
  the new disk without touching any header.
* `__empty` partitions are copied header-only; their payload is free space.

## Safety

| Rule | Behaviour |
| --- | --- |
| Source must be clean | Damaged chains are refused; run the health check and repair first |
| No PC-owned sources | GPT/foreign MBR refused; APA-Jail refused (its exFAT sits at a fixed offset) |
| Physical targets | Must be blank (no MBR signature, no GPT) or already a PS2 disk, never the source; typed `PhysicalDriveN` confirmation of the **target** |
| Image targets | Always a new file (never overwritten), created sparse |
| Order | All ranges except sectors 0-1, flush, SHA-256 verify, then the APA master |
| Interruption | No master on the target, so no PS2 tool accepts it as a disk; rerun to finish |
| Stale plans | The source master is re-read and compared before the first byte is copied |

Fault injection (`tests/fault_injection_tests.cpp`) cuts power at every write
of a migration and proves the target is either complete or not an APA disk.

## Commands

```
ps2-driveforge-repair-tools migrate disk.img bigger.img 1T [--apply]
ps2-driveforge-physical-tools backup 2 "PS2 HDD backup.img" [size]
ps2-driveforge-physical-tools migrate 2 3 --apply --confirm PhysicalDrive3
ps2-driveforge-physical-tools restore-image backup.img 3 --apply --confirm PhysicalDrive3
```

WinUI: HDL Tools > *Move to a bigger drive / back up*. Win32: *Recovery >
Back up / clone to a new image* (physical targets are WinUI-only).
