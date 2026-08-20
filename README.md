# PS2 DriveForge

**Modern APA/PFS HDD management for PlayStation 2.**

Current development train: **0.3.x — “Chisato”**  
Current source version: **0.3.0-dev**

PS2 DriveForge is a Windows-first PS2 HDD management stack. It is being built as a safe, testable alternative to the old shell-oriented workflow around `pfsshell`: one core parser/reader shared by a native GUI, CLI, host export layer, and later a Dokany Explorer provider.

> **Safety status:** source devices remain read-only. There is no public HDD write API and the Windows physical-drive backend requests `GENERIC_READ` only. Export writes only to user-selected host-side files/directories.

## Why another APA/PFS tool?

`pfsshell` remains useful and is one of DriveForge's compatibility references. DriveForge deliberately changes the host-side architecture rather than trying to wrap the interactive shell model in a prettier frontend.

The core differences today are:

- no process-global selected-device/current-mount/current-directory model in the parser;
- a byte-addressed `BlockDevice` boundary instead of exposing iomanX semantics to frontends;
- APA main/sub-partition translation isolated in `ApaVolume`;
- explicit `PFS Reader + Node + offset + span` byte-range reads;
- GUI/CLI/host export sharing the same parser instead of reproducing filesystem behavior;
- structural read-only safety while the parser is still maturing.

This does **not** mean DriveForge has already benchmarked faster throughput than pfsshell/pfsfuse. Current physical-drive I/O is synchronous and deliberately conservative. The performance plan and the rules for making honest comparisons are documented in [`docs/performance.md`](docs/performance.md) and [`docs/pfsshell-comparison.md`](docs/pfsshell-comparison.md).

## What works now

- open PS2 HDD images on Windows, Linux and macOS;
- open `\\.\PhysicalDriveN` read-only on Windows;
- detect and validate the APA v2 MBR;
- enumerate and diagnose APA partition chains;
- recognize MBR, PFS, HDL and free partitions;
- account for APA sub-partitions and non-contiguous extents;
- probe PFS primary/backup superblocks and validate zone size;
- read and checksum PFS SEGD inodes;
- follow indirect SEGI descriptor chains for large/fragmented files;
- enumerate PFS directories and resolve paths;
- read arbitrary byte ranges, including cross-sector ranges;
- browse PFS partitions from the CLI;
- recursively export PFS files/directories to the host;
- sanitize PS2 filenames for Windows host restrictions;
- detect export directory cycles and host-name collisions;
- native Windows GUI with APA partition tree and PFS file browser;
- double-click file extraction from the GUI;
- build/test on Windows x64 with MSVC;
- run Clang ASan + UBSan with warnings-as-errors in CI.

Ayanami and Bocchi were validated against a real **149.05 GiB PS2 HDD**. Bocchi successfully enumerated the real `+OPL` root (`CFG`, `THM`, `LNG`, `ART`, `VMC`, `CHT`, `APPS`) and correctly reported the empty `CFG` directory. See [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md).

## Current limitations

These are deliberate or known gaps, not hidden TODOs:

- source HDD/image mutation is not implemented;
- Chisato's GUI is the first functional browser shell, not the final UX;
- recursive export has not yet been hardware-validated on a non-empty PFS tree;
- Windows physical-drive reads are currently synchronous and serialized per device;
- no inode/directory/block cache exists yet;
- no read-ahead or overlapped I/O exists yet;
- Dokany Explorer mounting belongs to the next release train (`0.4 Darkness`);
- HDL virtual ISO browsing/import/export is a later milestone;
- project licensing remains intentionally TBD until the upstream-code/definition audit is complete.

## Windows GUI

The Windows package contains `PS2-DriveForge.exe` in addition to the CLI inspector.

Current GUI behavior:

- `File -> Open disk image...` opens a raw PS2 HDD image;
- `File -> Open physical drive -> PhysicalDriveN` opens a physical disk read-only;
- the left pane lists APA main partitions;
- selecting a PFS partition opens its root directory in the right pane;
- double-click a folder to navigate into it;
- use `..` to navigate upward;
- double-click a regular file to extract it with a normal Windows **Save As** dialog;
- status text identifies the current PFS path and read-only state.

The GUI owns no APA/PFS parser logic. It consumes the same core API as the CLI and future filesystem-provider code.

## Windows x64 build

Install Visual Studio 2022 with **Desktop development with C++**, including MSVC, Windows 10/11 SDK and CMake tools. In Developer PowerShell:

```powershell
.\build-windows.ps1 -Clean
```

Output is placed in `dist\windows-x64` and packaged as:

```text
PS2-DriveForge-0.3.0-Chisato-Release-windows-x64.zip
```

GitHub Actions performs the same MSVC build automatically for pull requests and `main`.

## Generic CMake build

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## CLI

### Inspect an image

```bash
./build/ps2-driveforge-inspect disk.img
```

### Inspect a physical drive on Windows

Run an elevated terminal if Windows requires it and always verify the disk number first:

```powershell
.\ps2-driveforge-inspect.exe --physical 3
```

### Browse a PFS partition

```powershell
.\ps2-driveforge-inspect.exe --physical 3 --browse +OPL
.\ps2-driveforge-inspect.exe --physical 3 --browse +OPL CFG
```

The same commands work with an image path instead of `--physical N`.

### Export a PFS file or directory

Single file:

```powershell
.\ps2-driveforge-inspect.exe --physical 3 --extract +OPL CFG/SLUS_123.45.cfg game.cfg
```

Whole directory tree:

```powershell
.\ps2-driveforge-inspect.exe --physical 3 --extract +OPL / exported-OPL
```

Directory export is recursive, streams files in 1 MiB host-side chunks, converts illegal Windows filename characters, protects reserved DOS device names such as `CON`/`NUL`/`LPT1`, avoids host-side case-insensitive name collisions, and refuses PFS directory cycles. The PS2 source is never opened for writing.

## Test coverage

Current synthetic coverage includes:

- valid and invalid APA chains;
- APA checksum and cycle detection;
- PFS primary/backup superblocks;
- root inode and directory enumeration;
- path resolution;
- unaligned reads crossing a 512-byte sector boundary;
- indirect SEGI descriptor traversal;
- Windows-safe host filename conversion.

CI runs the tests both under Windows/MSVC and under Clang with AddressSanitizer + UndefinedBehaviorSanitizer.

## Developer documentation

Start here when changing core behavior rather than guessing from old chat/commit history:

| Document | Purpose |
| --- | --- |
| [`docs/architecture.md`](docs/architecture.md) | Layer boundaries and dependency direction |
| [`docs/apa-format-notes.md`](docs/apa-format-notes.md) | APA units, links, checksums and main/sub extents |
| [`docs/pfs-format-notes.md`](docs/pfs-format-notes.md) | PFS zones, metadata, SEGD/SEGI and dentry traps |
| [`docs/pfsshell-comparison.md`](docs/pfsshell-comparison.md) | What we changed relative to the pfsshell architecture |
| [`docs/performance.md`](docs/performance.md) | Current bottlenecks, instrumentation and benchmark plan |
| [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md) | What has actually worked on a physical PS2 HDD |
| [`docs/development-guidelines.md`](docs/development-guidelines.md) | Documentation/comment/testing definition of done |
| [`docs/release-codenames.md`](docs/release-codenames.md) | The regrettably permanent anime release train |

## Roadmap

1. **0.1 “Ayanami”** — APA read-only core, diagnostics and initial PFS probing. **Done.**
2. **0.2 “Bocchi”** — PFS inode/directory/file read path. **Done / hardware validated.**
3. **0.3 “Chisato”** — native Windows GUI browser + reusable host export. **In progress.**
4. **0.4 “Darkness”** — Dokany Explorer mount, read-only first.
5. **0.5 “Emilia”** — cache, read-ahead and overlapped-I/O performance pass.
6. **0.6 “Frieren”** — carefully gated PFS/APA write path with automatic metadata backup.
7. **0.7 “Guts”** — HDL game view and virtual ISO import/export.
8. **0.8 “Holo”** — PS2 MBR/boot manager/recovery integration.

Release train names proceed alphabetically by anime character. Patch releases inherit the codename of their minor release.

## Architecture

```text
 Native Win32 GUI        CLI        future Dokany
        |                 |                 |
        +-------- ps2driveforge_host -------+
                          |
                 ps2driveforge_core
                          |
               +----------+----------+
               |          |          |
              APA        PFS       HDL/MBR
               |          |          |
               +----------+----------+
                          |
                     BlockDevice
                   /             \
             disk image      PhysicalDriveN
```

Format code does not know about Win32 controls, Windows filenames, or Dokany. Frontends do not implement APA/PFS parsing.

## Format references

APA/PFS layout and behavior are checked against current PS2SDK `libapa`, `libpfs` and related headers. `pfsshell` remains a compatibility reference, but DriveForge deliberately does not adopt its stateful shell/iomanX frontend architecture.

Upstream references:

- <https://github.com/ps2dev/ps2sdk>
- <https://github.com/ps2homebrew/pfsshell>

## License

**TBD during the private prototype phase.** Before public release we will audit retained upstream definitions/code and choose a compatible project license.
