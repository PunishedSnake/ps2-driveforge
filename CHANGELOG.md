# Changelog

## 0.5.0 "Emilia" - 2026-08-23

- Started the performance/frontend-modernization release above the hardware-validated Darkness read-only Explorer stack.
- Added backing service-time, small-read and maximum-in-flight instrumentation so performance work can separate request count from actual device latency/concurrency.
- Added immutable session caches for validated PFS probes, nodes/inodes, directory listings and stat results with explicit cold-cache reset semantics.
- Added a bounded 4 KiB small-read window cache and adaptive sequential read-ahead with usefulness/waste counters.
- Replaced shared seek-pointer physical/image I/O with explicit-offset operations: Windows uses OVERLAPPED request offsets; POSIX images use `pread()`.
- Added Windows storage-characteristic hints for rotational/solid-state/unknown media, seek penalty, TRIM, bus and optional ATA nominal rotation rate without treating missing hints as errors.
- Added zero-I/O `PartitionCatalog` construction from an existing APA scan; filtering/sorting/selecting the management list no longer requires payload probes.
- Added native read-only HDLoader metadata parsing from the main APA partition at `+0x101000` (`0xDEADFEED`) instead of spawning `HDL.EXE` per game.
- Added physical-LBA-ordered progressive/cancellable HDL enrichment plus a frontend-neutral `ManagementModel` whose base rows exist before optional title/detail enrichment completes.
- Added measured HDL queue-depth policy and developer-only `--hdl-qd N` overrides; normal `unknown`/rotational behavior remains conservative rather than copying the fastest setting from one disk.
- Added `ps2-driveforge-benchmark` support for cold APA/catalog, PFS browse+stat cold/warm behavior, native HDL enrichment and storage/I/O counters.
- Recorded the first full real-HDD Emilia sweep: 190-header cold APA median 1555.022 ms, zero-I/O 190-row catalog median 0.006 ms, cold `+OPL` browse+stat 10 reads/16 KiB with a zero-backing-read warm repeat, and 35/35 native HDL rows readable.
- Preserved the automatic `unknown`-media QD1 policy despite QD8 completing the narrow 35-title workload ~9.6% faster on the validation HDD because QD8 multiplied average read service latency and management rows are already usable before enrichment finishes.
- Added native WinUI 3 / C++/WinRT source using the shared DriveSession/PartitionCatalog/ManagementModel backend rather than a second parser stack.
- Added a self-contained unpackaged WinUI build to canonical Windows CI/package staging while retaining the hardware-validated Win32 frontend as the supported fallback.
- Fixed WinUI release staging so build/CI discover the actual MSBuild output directory instead of assuming `src\winui\x64\Release\PS2-DriveForge-WinUI.exe` is flat; current MSBuild emits a project-name subdirectory.
- Expanded the normal regression suite to **14 executables** covering read cache, read-ahead, partition catalog/management model, HDL enrichment, storage policy and existing APA/PFS/session/Dokany/Darkness contracts.
- Merged the stable Emilia functionality baseline to `main` as commit `210d598442641291f12ecb03323d8a91586b6dfd` after Windows + Dokany + WinUI and Linux sanitizer CI were green and the canonical artifact was smoke-checked.
- Began 0.5 release-prep cleanup on a separate branch so the stable functionality baseline remains available for regression comparison.
- Moved native HDL parsing and zero-I/O partition-catalog implementation out of public headers into owned `src/core` translation units, and moved ManagementModel mutation/construction code into `src/host` without changing behavior.
- Added deterministic `scripts/verify-windows-package.ps1` validation of required release binaries/docs, the exact 14 packaged regression executables and the WinUI payload.
- Added reproducible Windows/Linux build documentation, refreshed architecture/testing/contributing/development docs for Emilia, and added explicit RC/release gates plus a real-Windows/real-PS2-HDD checklist.
- Changed automatic Explorer mount-letter policy from a hard/preferred `P:` scheme to the **lowest currently free letter from C: through Z:** while always excluding A: and B:, with deterministic regression coverage.
- Fixed the unpackaged WinUI self-contained deployment so the release payload includes the actual Windows App SDK/WinUI runtime rather than a misleading seven-file frontend-only directory; release verification now requires `Microsoft.UI.Xaml.dll` and Windows App Runtime components.
- Added very-early WinUI startup diagnostics under `%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log`, including XAML/WinRT `HRESULT`, native unhandled-exception and activation stage markers.
- Fixed real-machine WinUI XAML startup failures first exposed as missing `AccentFillColorDefaultBrush`, then as missing `TabViewButtonBackground`; DriveForge now owns its application semantic brushes and merges `Microsoft.UI.Xaml.Controls.XamlControlsResources` for control-internal Fluent resources.
- Added a clean user-facing release layout with a small root `PS2-DriveForge.exe` launcher, WinUI/runtime under `app\winui`, the supported Win32 fallback under `legacy`, tools under `tools`, and regression executables kept out of normal user packages.
- Added a post-`Window.Activate()` WinUI readiness handshake and `PS2-DriveForge.exe --legacy` direct fallback path so a modern-frontend startup failure does not strand the user without the validated Win32 interface.
- Fixed RC4's root-launcher loader crash caused by a `TaskDialogIndirect`/COMCTL32 ordinal import that could abort the process before `wWinMain()` and therefore before `--legacy` argument handling.
- Added `PS2-DriveForge.exe --self-test` and made release staging execute the **exact staged launcher** before building Portable/Setup artifacts, turning loader/import failures into CI packaging failures.
- Fixed Win32 dark-theme status-bar readability by custom-painting the status client with the active DriveForge dark palette while preserving normal Light/System/High-Contrast behavior.
- Added Inno Setup installer generation alongside the clean Portable ZIP, including explicit Start Menu access to the supported Legacy Win32 fallback and optional verified Dokany prerequisite installation.
- Added an MIT license for original DriveForge code, public credits/acknowledgements and third-party notices; user packages now require these files and the installer displays the project license.
- Updated the public WinUI dependency from the affected Windows App SDK 2.3.1 / WinUI 2.3.0 combination to Windows App SDK 2.4.0 and repeated self-contained package/startup validation.
- Completed the functional WinUI pass on real hardware: launcher and custom title bar, UAC restart, automatic PS2 HDD discovery, APA/HDL/PFS browsing, Files, Performance, Settings, read-only Dokany Explorer mount and Win32 fallback all use the shared native backend.
- Unified HDD Manager, Files, Mount, Performance and Settings behind one viewport-aware responsive page host; narrow windows scale correctly and the remaining wide-window density is cosmetic rather than a release blocker.
- Finalized the Windows pipeline to produce and hash `PS2-DriveForge-0.5.0-Emilia-Portable-x64.zip` and `PS2-DriveForge-0.5.0-Emilia-Setup-x64.exe` as the public Emilia artifacts.

## 0.4.0-dev "Darkness" - 2026-08-21

- Started the read-only Dokany Explorer-mount release train above the hardware-validated Chisato stack.
- Added thread-safe `DriveSession::stat` and random-offset `DriveSession::read_file` operations for filesystem-provider callbacks.
- Made session operation counters atomic for concurrent Dokany workloads.
- Added portable `ReadOnlyMountView` mapping `\\Partitions\\...` into APA/PFS without making Dokany a parser dependency.
- Added deterministic Windows-safe mount aliases while preserving original APA/PFS names internally.
- Added mount-view regression coverage for root/partition enumeration, case-insensitive lookup, random-offset reads and EOF clamping.
- Added `PS2-DriveForge-Mount.exe` with image and read-only `PhysicalDriveN` sources plus explicit unmount support.
- Implemented Dokany 2.3.1 callbacks for create/open, read, metadata, directory enumeration, volume/free-space queries and mount lifecycle.
- Added structural write protection through `DOKAN_OPTION_WRITE_PROTECT`, mutation callbacks returning write-protected status, the absence of a `BlockDevice::write()` API, and continued `GENERIC_READ` physical-drive access.
- Pinned Windows CI to the official Dokany 2.3.1 x64 MSI, verifies its SHA-256, installs development headers/libs, builds the mount frontend and packages it with the GUI/CLI.
- Added a CMake `FindDokany.cmake` path and opted into CMP0144 when available so explicit `DOKANY_ROOT` builds remain warning-free on modern CMake.
- Added persistent native GUI theme selection under `View -> Theme -> System / Light / Dark`.
- System theme follows Windows `AppsUseLightTheme`; High Contrast overrides DriveForge theme choices for accessibility.
- Added explicit dark colours for the client area, TreeView, ListView/header and status bar, plus Windows 11 DWM dark-titlebar support.
- Added best-effort dynamically resolved UxTheme dark-menu/common-control support with a safe fallback if internal exports are unavailable.
- Removed the white class-background brush and paint the active palette in `WM_ERASEBKGND` to avoid bright flashes during dark-mode startup/resizing.
- Hardware-validated dynamic System/Light/Dark switching on Windows 11.
- The first real Dokany mount correctly created `P:` for `PhysicalDrive3`, but Explorer initially failed opening the root with `The file exists`.
- Fixed that root-open failure by separating Dokany/NT `FILE_OPEN`/`FILE_CREATE` disposition semantics from Win32 `OPEN_EXISTING`/`CREATE_NEW`; the numeric value `1` has different meanings in those APIs.
- Added portable `ps2-driveforge-dokany-open-policy-tests` coverage for existing-root `FILE_OPEN`, create collisions, missing-object opens, overwrite/create rejection, type mismatches and delete-on-close.
- Added `--debug` to `PS2-DriveForge-Mount.exe` for callback/path/disposition diagnostics during real-machine Explorer testing.
- Hardware-validated the corrected read-only mount on the real 149.05 GiB APA v2 HDD: `P:\`, `P:\Partitions`, `+OPL`, and `__common\OPL` browse correctly; Explorer-copied `conf_hdd.cfg` matches SHA-256 `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`; write creation is rejected; unmount is clean.
- Replaced the GUI's fixed `PhysicalDrive0..31` list with SetupAPI `GUID_DEVINTERFACE_DISK` enumeration and `IOCTL_STORAGE_GET_DEVICE_NUMBER` mapping to actual raw-disk numbers.
- Automatic discovery now probes only real Windows disk interfaces and classifies them with the normal DriveForge APA parser; the GUI presents only PS2 APA candidates with friendly name, capacity, APA version and main-partition count.
- Added asynchronous PS2 HDD discovery at GUI startup plus `Rescan PS2 HDDs`; when exactly one PS2 APA candidate exists it is opened automatically.
- Extended `PhysicalDrive` diagnostics with the Win32 raw-open error so access-denied and nonexistent/unavailable device cases are distinguishable without changing `GENERIC_READ` semantics.
- Added controlled UAC `runas` relaunch for normal-user GUI startup, a `--elevated-relaunch` loop-prevention marker, cancellation fallback to limited image-capable mode, and `Restart as Administrator` in the GUI.
- Moved Dokany callbacks and lifecycle into shared `DokanyMountController`; the GUI and standalone mount CLI now use the same filesystem-provider implementation instead of duplicating callbacks or spawning a helper process.
- Added direct GUI `Mount read-only`, `Open mounted volume in Explorer`, and `Unmount` actions.
- Added automatic free-drive-letter selection for GUI mounts, preferring `P:` and falling back to another free data-drive letter.
- Retained `PS2-DriveForge-Mount.exe` as a thin diagnostic/script frontend over the shared controller.
- Added portable Darkness GUI/mount policy coverage for one-candidate auto-open behavior, source-preserving rescan behavior, `P:` preference, occupied-letter fallback, and exhaustion of `D:`-`Z:`.
- Expanded the normal regression matrix to **nine** test executables; Linux Clang ASan/UBSan and Windows/MSVC + Dokany remain the merge gates.
- Recorded the corrected real-HDD Dokany mount, mounted copy/hash integrity, write rejection and clean unmount in `REAL_HARDWARE_VALIDATION.md`.
- Defined the final Darkness-to-Emilia Explorer workload so metadata/open/enumeration behavior can be compared before and after 0.5 caching/read-ahead/overlapped-I/O work.
- The remaining Darkness gate is a real-machine smoke test of the new integrated GUI flow: normal-user launch -> UAC relaunch -> automatic SetupAPI discovery -> automatic PS2 HDD open -> GUI mount/open/unmount, plus cancellation/rescan/free-letter fallback checks.

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
