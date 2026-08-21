# Changelog

## 0.4.0-dev "Darkness" - 2026-08-21

- Started the read-only Dokany Explorer-mount release train above the hardware-validated Chisato stack.
- Added thread-safe `DriveSession::stat` and random-offset `DriveSession::read_file` operations for filesystem-provider callbacks.
- Made session operation counters atomic for concurrent Dokany workloads.
- Added portable `ReadOnlyMountView` mapping `\\Partitions\\...` into APA/PFS without making Dokany a parser dependency.
- Added deterministic Windows-safe mount aliases while preserving original APA/PFS names internally.
- Added mount-view regression coverage for root/partition enumeration, case-insensitive lookup, random-offset reads and EOF clamping.
- Added `PS2-DriveForge-Mount.exe` with image and read-only `PhysicalDriveN` sources plus explicit unmount support.
- Implemented the initial Dokany 2.3.1 callbacks for create/open, read, metadata, directory enumeration, volume/free-space queries and mount lifecycle.
- Added structural write protection through `DOKAN_OPTION_WRITE_PROTECT`, mutation callbacks returning write-protected status, the absence of a `BlockDevice::write()` API, and continued `GENERIC_READ` physical-drive access.
- Pinned Windows CI to the official Dokany 2.3.1 x64 MSI, verifies its SHA-256, installs development headers/libs, builds the mount frontend and packages it with the GUI/CLI.
- Added a CMake `FindDokany.cmake` path and opted into CMP0144 when available so explicit `DOKANY_ROOT` builds remain warning-free on modern CMake.
- Added persistent native GUI theme selection under `View -> Theme -> System / Light / Dark`.
- System theme follows Windows `AppsUseLightTheme`; High Contrast overrides DriveForge theme choices for accessibility.
- Added explicit dark colours for the client area, TreeView, ListView/header and status bar, plus documented Windows 11 DWM dark-titlebar support.
- Added best-effort dynamically resolved UxTheme dark-menu/common-control support with a safe fallback if internal exports are unavailable.
- Removed the white class-background brush and paint the active palette in `WM_ERASEBKGND` to avoid bright flashes during dark-mode startup/resizing.
- Windows/MSVC + Dokany and Linux Clang ASan/UBSan CI remain green after the initial Darkness mount/theme work; Darkness still awaits real-HDD Explorer validation.

## 0.3.0-dev "Chisato" - 2026-08-21

- Added the first native Windows GUI (`PS2-DriveForge.exe`) using Win32/Common Controls only.
- Added Explorer-style APA partition tree and PFS details browser.
- Added image opening and read-only physical-drive opening from the GUI.
- Added read-only PS2 HDD discovery across `PhysicalDrive0..31` from both CLI and GUI.
- Added PFS folder navigation, folders-first sorting, `..` navigation, inode/size display and status-path reporting.
- Added GUI file extraction through the Windows Save As dialog.
- Added a reusable host export layer shared independently from the APA/PFS parser.
- Added recursive PFS directory export with 1 MiB streaming file reads.
- Added Windows-safe filename conversion, DOS device-name protection, case-insensitive collision handling, directory-depth limits and cycle detection.
- Extended CLI `--extract` so it can export either one file or an entire PFS directory tree.
- Added `DriveSession` as the shared frontend orchestration layer for APA scan, PFS browse, export and diagnostics.
- Refactored the Win32 GUI to use the same `DriveSession` and host exporter as the CLI instead of maintaining a duplicate PFS/copy path.
- Removed the forced image-open dialog on GUI startup; Chisato now starts with an explicit empty/read-only state.
- Added `InstrumentedBlockDevice` and CLI `--stats` backing-I/O diagnostics (read calls, bytes, average/largest read, failures, and session operation counts).
- Added a generated sparse APA/PFS disk-image end-to-end test that reopens the image through production `FileBlockDevice`.
- The generated image exercises nested/empty directories, Windows host-name collisions, an APA main/sub-partition data split, and a real SEGI-backed file.
- Added SHA-256 verification of every generated-image exported file and a known SHA-256 test vector.
- Added a deterministic malformed-metadata corpus covering out-of-device APA main/sub extents, invalid PFS zone sizes, missing root subpartitions, bad inode checksums and malformed dentries.
- Hardened the APA reader so main and recorded sub-partition extents extending beyond the backing device are fatal diagnostics.
- Added `DriveSession` regression tests for scan/browse/export and backing-I/O counter reset/collection.
- Added an optional Clang/libFuzzer APA parser target (`PS2DF_BUILD_FUZZERS=ON`).
- Expanded the normal CI suite to seven test executables; Windows/MSVC and Clang ASan+UBSan+`-Werror` both pass the complete suite.
- Cleaned the Win32 control-ID casts so MSVC builds without DriveForge `Cxxxx` warnings.
- Made the Win32 GUI target compile source text explicitly as UTF-8 under MSVC, fixing mojibake in punctuation used by window/status/discovery strings.
- Windows artifacts package both the GUI and CLI inspector.
- Expanded README into the project entry point with current limitations, source detection, statistics and developer-documentation index.
- Added APA and PFS implementation notes documenting address units, checksums, extent translation, SEGD/SEGI behavior and directory-entry traps.
- Added an architecture comparison with pfsshell that separates observed differences from unbenchmarked performance targets.
- Added a performance/benchmark plan and implemented its first backing-I/O instrumentation layer.
- Added `docs/testing.md` for generated-image E2E, corruption corpus, fuzzing, statistics and real-HDD workflow.
- Added development/documentation rules so format decisions, hardware findings and non-obvious invariants are recorded alongside code changes.
- Expanded real-hardware validation notes into reusable regression cases and explicit coverage gaps.
- Added code comments around APA traversal/checksums, PFS zone/metadata arithmetic, SEGI traversal, dentry boundaries, host filename policy and Windows raw-disk serialization.
- Hardware-validated Chisato on a 149.05 GiB APA v2 HDD: read-only discovery found `PhysicalDrive3` as the sole PS2 APA candidate with 190 headers; full scan and GUI reported clean diagnostics and 43 main partitions.
- Recorded the first real-HDD backing-I/O baseline for `+OPL` browse: 215 backing reads / 206.50 KiB, 983 B average, 1 KiB largest, zero failed reads.
- Hardware-validated recursive export of the real `+OPL` directory tree.
- Hardware-validated regular-file extraction from `__common:/OPL/conf_hdd.cfg` (20 bytes, SHA-256 `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`).
- Real-HDD PFS SEGI traversal remains an explicit coverage gap because this test disk stores its large content as HDL game partitions; deterministic generated-image SEGI coverage remains green.
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
