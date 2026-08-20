# Real hardware validation

PS2 DriveForge 0.1.0-dev "Ayanami" has been validated against a real 149.05 GiB APA v2 PlayStation 2 HDD through the Windows `PhysicalDrive` backend.

Observed successfully:

- APA v2 MBR detection and full linked-list traversal
- PFS v3 detection on system and user partitions
- 8 KiB PFS zone-size validation
- matching primary and backup PFS superblocks
- HDL main/sub partition association, including non-contiguous sub-partitions
- read-only Windows raw-disk access

The first hardware report included `__net`, `__system`, `__sysconf`, `__common`, `__boot`, `HDLoader Settings`, `+OPL`, and many HDL game partitions.

The next validation milestone is PFS directory enumeration and file reading on the same physical disk.
