# Changelog

## 0.2.0-dev "Bocchi" - 2026-08-20

- Added native 1024-byte PFS inode parsing with checksum and SEGD validation.
- Added chained SEGI indirect segment descriptor support for heavily fragmented/large files.
- Added PFS data-extent reads across arbitrary byte ranges and APA sub-partitions.
- Added 512-byte PFS directory-entry parsing and path resolution.
- Added `--browse <partition> [path]` directory browsing.
- Added `--tree <partition> [path]` recursive filesystem traversal with cycle protection.
- Added `--extract <partition> <path> <host-file>` read-only file export.
- Added dedicated PFS filesystem tests, including nested paths, multi-sector directories and SEGI-backed data.
- Added thread-safe inode/directory metadata caches for repeated frontend queries.
- Hardened directory-entry bounds validation.
- Moved Windows CI/package naming to the Bocchi development train.
- Ayanami's APA/PFS probe path was validated against a real APA v2 / PFS v3 PS2 HDD before starting this train.

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
