# Changelog

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
