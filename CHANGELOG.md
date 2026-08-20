# Changelog

## 0.2.0-dev "Bocchi" - 2026-08-20

- Added the first native read-only PFS inode reader.
- Added PFS inode checksum and SEGD/SEGI magic validation.
- Added zone/inode address translation matching PS2SDK semantics.
- Added direct PFS data-segment reads across APA main/sub extents.
- Added chained indirect SEGI descriptor traversal for large or heavily fragmented files.
- Added PFS directory-entry parsing with 512-byte boundary validation.
- Added path resolution from the PFS root inode.
- Added `--browse <partition> [path]` to enumerate PFS directories.
- Added streaming `--extract <partition> <pfs-path> <output>` with 1 MiB host-side chunks.
- Added synthetic root-directory, path-resolution, cross-sector file-read, and SEGI traversal coverage.
- Added Clang ASan + UBSan CI with warnings-as-errors.
- Kept physical HDD access strictly read-only; extraction only writes the selected host output file.

## 0.1.0-dev "Ayanami" - 2026-08-20

- Established the project identity as **PS2 DriveForge**.
- Added semantic version/codename build metadata and `--version` output.
- Added one-command Windows x64 build/test/package script (`build-windows.ps1`).
- Added GitHub Actions `windows-2022` CI producing a downloadable Windows package.
- Added byte-addressed read-only `BlockDevice` abstraction.
- Added image-backed block device.
- Added Windows read-only `PhysicalDriveN` backend.
- Added native APA header definitions and checksum validation.
- Added APA MBR detection and partition-chain traversal.
- Added cycle, bounds, checksum and link diagnostics.
- Added partition type display and sub-partition accounting.
- Added `ps2-driveforge-inspect` CLI.
- Added synthetic tests for valid chains, bad checksums, cycles and PFS probing.
- Added APA logical-volume extent translator for main/sub partitions.
- Added read-only PFS primary/backup superblock probing and validation.
