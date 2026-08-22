# PS2 DriveForge

**A modern, read-only Windows toolkit for PlayStation 2 APA/PFS hard drives.**

PS2 DriveForge lets you inspect a PS2 HDD, browse PFS filesystems, inspect HDLoader game metadata and expose supported content to Windows Explorer without turning a decades-old disk format into a shell-script exercise.

The current development train is **0.5.x — Emilia**. The active release candidate keeps the proven Win32 interface as a supported fallback while the new WinUI frontend is validated on real Windows systems.

> [!IMPORTANT]
> DriveForge is deliberately **read-only**. Physical disks are opened with `GENERIC_READ`; the public block-device API has no write operation; Dokany mounts are write-protected and mutation callbacks are rejected. A damaged PS2 HDD should be imaged before any recovery work.

## Why DriveForge exists

PS2 HDD tooling is powerful, but much of the traditional workflow assumes command-line knowledge, manual device selection and familiarity with APA/PFS internals. DriveForge puts the same low-level formats behind a native, testable C++20 storage stack and normal Windows UX.

Emilia focuses on three things:

- **safety** — strict metadata validation and read-only physical access;
- **responsiveness** — scan APA once, build the management catalog in memory, enrich optional metadata progressively;
- **normal desktop behavior** — automatic PS2 HDD discovery, Explorer mounting, modern UI, a working legacy fallback and conventional installer/portable packages.

## Current features

- APA v2 MBR/checksum/link traversal with main/sub-partition validation;
- PFS v3 superblock, inode, directory and SEGI indirect-descriptor reading;
- native HDLoader `0xDEADFEED` metadata parsing;
- disk-image and Windows `PhysicalDriveN` backends;
- SetupAPI disk discovery followed by actual APA classification;
- recursive read-only export to the host filesystem;
- Dokany 2.3.1 read-only Explorer mount;
- zero-I/O `PartitionCatalog` after the validated APA scan;
- frontend-neutral `ManagementModel` and native HDL enrichment;
- bounded small-read cache, read-ahead and backing-I/O instrumentation;
- WinUI 3 frontend with startup diagnostics;
- supported Win32 fallback with System/Light/Dark themes;
- inspection and benchmark CLIs;
- 14 deterministic regression executables plus Linux ASan/UBSan CI;
- real-PS2-HDD validation and measured Emilia performance baselines.

## Windows release layout

The recommended distribution is the installer:

```text
PS2-DriveForge-0.5.0-Emilia-Setup-x64.exe
```

A portable package is also produced. Its root is intentionally kept human-readable:

```text
PS2-DriveForge.exe          default launcher
README.md
CHANGELOG.md
LICENSE
CREDITS.md
THIRD_PARTY_NOTICES.md

app\winui\                 modern frontend and its runtime
legacy\                    supported Win32 fallback
tools\                     inspector / benchmark / mount diagnostics
docs\                      detailed documentation
```

`PS2-DriveForge.exe` starts the WinUI frontend and waits for a post-activation readiness handshake. If WinUI fails before its main window becomes ready, the launcher offers the Win32 fallback and the startup log instead of silently disappearing.

You can also bypass WinUI explicitly:

```powershell
.\PS2-DriveForge.exe --legacy
```

The Win32 frontend is **supported fallback software**, not a deprecated copy. Both frontends share the same native DriveForge storage/host layers.

## WinUI diagnostics

During the 0.5 release-candidate cycle WinUI writes an early startup log to:

```text
%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log
```

The log covers process/static initialization, `App`, XAML initialization, `MainWindow`, activation, WinRT `HRESULT`s and native unhandled exceptions. See [`docs/winui-diagnostics.md`](docs/winui-diagnostics.md).

## Mounting behavior

When DriveForge creates a read-only Explorer mount it selects the **lowest free drive letter from C: through Z:**. A: and B: are never selected. This follows normal removable-storage expectations rather than reserving a hard-coded `P:` drive.

## Performance model

The HDD-manager path is intentionally simple:

```text
one validated APA scan
   -> zero-I/O PartitionCatalog
   -> complete management rows immediately
   -> filter / sort / select in memory
   -> optional progressive HDL/PFS enrichment
```

On the first complete real-HDD Emilia benchmark, a 190-row partition catalog built in a median **0.006 ms after the APA scan with zero additional backing reads**. That number describes one validation disk, not a universal promise. Full measurements and methodology live in [`docs/emilia-benchmark-2026-08-22.md`](docs/emilia-benchmark-2026-08-22.md).

## Build

The normal Windows developer/release build is driven by one PowerShell entry point:

```powershell
.\build-windows.ps1 `
  -Configuration Release `
  -Clean `
  -WithDokany `
  -DokanyRoot 'C:\Program Files\Dokan\DokanLibrary-2.3.1'

.\scripts\verify-windows-package.ps1
```

The user-facing installer/portable staging is generated separately so regression executables and runtime internals do not clutter the normal download.

Portable Linux core/host tests:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPS2DF_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

See [`BUILDING.md`](BUILDING.md) for Visual Studio, Windows App SDK, Dokany, Inno Setup, sanitizer and package details.

## Architecture

```text
                 PS2-DriveForge.exe launcher
                         |
                +--------+--------+
                |                 |
             WinUI 3          Win32 fallback
                |                 |
                +--------+--------+
                         |
                  ps2driveforge_host
                    /      |      \
             DriveSession  |  ManagementModel
                           |
                    PartitionCatalog
                           |
                  ps2driveforge_core
                    APA / PFS / HDL
                           |
                cache / read-ahead / stats
                           |
                  image / PhysicalDrive

Explorer <-> Dokany read-only provider <-> shared host/core
```

Format parsing stays below platform/UI code. Frontends do not reimplement APA, PFS or HDLoader parsing. See [`docs/architecture.md`](docs/architecture.md) and [`docs/windows-frontends.md`](docs/windows-frontends.md).

## Safety and real-hardware testing

A green CI build is not enough to call a storage tool safe. Release candidates are additionally tested against a real PS2 HDD for:

- automatic discovery and read-only open;
- known APA/PFS/HDL catalog contents;
- Explorer mount/open/unmount/remount;
- rejected write/create/delete/rename operations;
- fallback drive-letter selection;
- WinUI startup and Win32 fallback behavior;
- dark/light theme readability;
- clean process/device teardown.

The exact checklist is [`docs/rc-hardware-checklist.md`](docs/rc-hardware-checklist.md). Historical hardware evidence is preserved in [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md).

## Current limitations

- DriveForge intentionally does not modify source HDDs or disk images.
- WinUI is still undergoing real-machine release-candidate validation; Win32 remains the supported fallback.
- The final least-privilege architecture still calls for a narrow elevated raw-disk broker rather than keeping a full GUI elevated.
- Wider HDD/SSD/USB-bridge samples are needed before changing conservative unknown-media I/O defaults.
- HDL virtual ISO browsing/import/export is outside the current 0.5 scope.
- The private RC5 WinUI build still resolves Windows App SDK 2.3.1 / WinUI 2.3.0. Microsoft identifies that WinUI package as affected by an incorrect non-redistributable license and instructs affected publishers to update to **Windows App SDK 2.4.0**, which resolves WinUI 2.3.6. Therefore the public 0.5 candidate must perform that dependency bump and repeat WinUI/package/hardware startup validation; see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Documentation

Start with [`docs/README.md`](docs/README.md) for the documentation index. Important references include:

- [`BUILDING.md`](BUILDING.md) — reproducible builds and packaging;
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — development and safety rules;
- [`docs/architecture.md`](docs/architecture.md) — layer ownership and dependencies;
- [`docs/windows-frontends.md`](docs/windows-frontends.md) — launcher, WinUI, Win32 fallback and package layout;
- [`docs/testing.md`](docs/testing.md) — regression/CI test matrix;
- [`docs/release-process.md`](docs/release-process.md) — release gates;
- [`docs/rc-hardware-checklist.md`](docs/rc-hardware-checklist.md) — real-HDD RC validation;
- [`docs/apa-format-notes.md`](docs/apa-format-notes.md) and [`docs/pfs-format-notes.md`](docs/pfs-format-notes.md) — format notes;
- [`docs/performance.md`](docs/performance.md) — instrumentation policy;
- [`docs/winui-diagnostics.md`](docs/winui-diagnostics.md) — startup logging and crash triage.

## License and acknowledgements

Original PS2 DriveForge source code is licensed under the **MIT License**; see [`LICENSE`](LICENSE).

DriveForge relies on and interoperates with independently licensed software, and its format work was cross-checked against long-standing PS2 homebrew projects. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for legal notices and [`CREDITS.md`](CREDITS.md) for acknowledgements.

PlayStation and related marks are trademarks of their respective owners. PS2 DriveForge is an independent homebrew project and is not affiliated with or endorsed by Sony Interactive Entertainment.
