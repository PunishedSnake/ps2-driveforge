# PS2 DriveForge

**Modern APA/PFS HDD management for PlayStation 2.**

Current development train: **0.2.x — “Bocchi”**  
Current source version: **0.2.0-dev**

PS2 DriveForge is a Windows-first replacement for the old `pfsshell` style of PS2 HDD management. The goal is a safe, fast core plus a native GUI that can inspect and manage APA/PFS/HDL disks and eventually expose their contents naturally to Windows Explorer.

> **Safety status:** source devices remain read-only. There is no public HDD write API and the Windows physical-drive backend requests `GENERIC_READ` only. File extraction writes only to a user-selected host-side output file.

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
- stream PFS files to the host without loading the whole file into memory;
- build/test on Windows x64 with MSVC;
- run Clang ASan + UBSan with warnings-as-errors in CI.

Ayanami was also validated against a real 149.05 GiB PS2 HDD containing PFS system partitions, `+OPL`, and many fragmented HDL game partitions. See [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md).

## Windows x64 build

Install Visual Studio 2022 with **Desktop development with C++**, including MSVC, Windows 10/11 SDK and CMake tools. In Developer PowerShell:

```powershell
.\build-windows.ps1 -Clean
```

Output is placed in `dist\windows-x64` and packaged as:

```text
PS2-DriveForge-0.2.0-Bocchi-Release-windows-x64.zip
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

### Extract a PFS file

```powershell
.\ps2-driveforge-inspect.exe --physical 3 --extract +OPL CFG/SLUS_123.45.cfg game.cfg
```

Extraction streams the source in 1 MiB chunks. If a source read or host write fails, the partial destination is removed. The PS2 source device is never opened for writing.

## Test coverage

Current synthetic coverage includes:

- valid and invalid APA chains;
- APA checksum and cycle detection;
- PFS primary/backup superblocks;
- root inode and directory enumeration;
- path resolution;
- unaligned reads crossing a 512-byte sector boundary;
- indirect SEGI descriptor traversal.

CI runs the tests both under Windows/MSVC and under Clang with AddressSanitizer + UndefinedBehaviorSanitizer.

## Roadmap

1. **0.1 “Ayanami”** — APA read-only core, diagnostics and initial PFS probing. **Done.**
2. **0.2 “Bocchi”** — PFS inode/directory/file read path. **In progress.**
3. **0.3 “Chisato”** — native Windows GUI browser.
4. **0.4 “Darkness”** — Dokany Explorer mount, read-only first.
5. **0.5 “Emilia”** — cache, read-ahead and overlapped-I/O performance pass.
6. **0.6 “Frieren”** — carefully gated PFS/APA write path with automatic metadata backup.
7. **0.7 “Guts”** — HDL game view and virtual ISO import/export.
8. **0.8 “Holo”** — PS2 MBR/boot manager/recovery integration.

Release train names proceed alphabetically by anime character. Patch releases inherit the codename of their minor release. See [`docs/release-codenames.md`](docs/release-codenames.md).

## Architecture

`ps2driveforge_core` is intentionally independent from any GUI or filesystem-mount layer:

```text
Windows GUI / CLI / Dokany provider
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

See [`docs/architecture.md`](docs/architecture.md) for details.

## Format references

APA/PFS layout and behavior are checked against current PS2SDK `libapa`, `libpfs` and related headers. `pfsshell` remains a compatibility reference, but DriveForge deliberately does not adopt its stateful shell/iomanX frontend architecture.

## License

**TBD during the private prototype phase.** Before public release we will audit retained upstream definitions/code and choose a compatible project license.
