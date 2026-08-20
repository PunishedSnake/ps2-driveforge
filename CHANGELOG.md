# Changelog

## 0.3.0-dev "Chisato" - 2026-08-20

- Added the first native Windows GUI (`PS2-DriveForge.exe`) using Win32/Common Controls only.
- Added Explorer-style APA partition tree and PFS details browser.
- Added image opening and read-only `PhysicalDrive0..15` opening from the GUI.
- Added PFS folder navigation, folders-first sorting, `..` navigation, inode/size display and status-path reporting.
- Added GUI file extraction through the Windows Save As dialog.
- Added a reusable host export layer shared independently from the APA/PFS parser.
- Added recursive PFS directory export with 1 MiB streaming file reads.
- Added Windows-safe filename conversion, DOS device-name protection, case-insensitive collision handling, directory-depth limits and cycle detection.
- Extended CLI `--extract` so it can export either one file or an entire PFS directory tree.
- Added host filename conversion tests.
- Windows artifacts now package both the GUI and CLI inspector.
- Expanded README into the project entry point with current limitations and developer-documentation index.
- Added APA and PFS implementation notes documenting address units, checksums, extent translation, SEGD/SEGI behavior and directory-entry traps.
- Added an architecture comparison with pfsshell that separates observed differences from unbenchmarked performance targets.
- Added a performance/benchmark plan that records the current synchronous I/O bottlenecks before optimization begins.
- Added development/documentation rules so format decisions, hardware findings and non-obvious invariants are recorded alongside code changes.
- Expanded real-hardware validation notes into reusable regression cases and explicit coverage gaps.
- Added code comments around APA traversal/checksums, PFS zone/metadata arithmetic, SEGI traversal, dentry boundaries, host filename policy and Windows raw-disk serialization.
- Source HDD/image access remains read-only.

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
- Hardware-validated `+OPL` root and empty `CFG` directory browsing on a real 149.05 GiB PS2 HDD.
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
