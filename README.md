# PS2 DriveForge

**Modern APA/PFS HDD management for PlayStation 2.**

Current development train: **0.1.x — “Ayanami”**  
Current source version: **0.1.0-dev**

PS2 DriveForge is a Windows-first replacement for the old `pfsshell` style of PS2 HDD management. The long-term goal is a safe, fast library and GUI that can inspect and manage APA/PFS/HDL disks and expose their contents naturally to Windows Explorer.

> **Safety status:** the current codebase is read-only. There is no public write API and the Windows physical-drive backend requests `GENERIC_READ` only.

## What works now

- open PS2 HDD images on Windows, Linux and macOS;
- open `\\.\PhysicalDriveN` read-only on Windows;
- detect and validate the APA MBR;
- enumerate APA partition headers;
- recognize MBR, PFS, HDL and free partitions;
- report broken checksums, invalid links, loops and out-of-range links;
- account for APA sub-partitions in logical partition size;
- translate PFS `sub + sector` addresses through APA extents;
- probe and validate PFS v1-v3 primary/backup superblocks and zone size;
- build and test on Windows x64 through one PowerShell command or GitHub Actions.

## Windows x64 build

Install Visual Studio 2022 with **Desktop development with C++**, including MSVC, Windows 10/11 SDK and CMake tools. In Developer PowerShell:

```powershell
.\build-windows.ps1 -Clean
```

Output is placed in `dist\windows-x64` and packaged as:

```text
PS2-DriveForge-0.1.0-Ayanami-Release-windows-x64.zip
```

GitHub Actions performs the same MSVC build automatically for pull requests and `main`.

## Generic CMake build

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Inspect an image

```bash
./build/ps2-driveforge-inspect disk.img
```

### Inspect a physical drive on Windows

Run an elevated terminal if Windows requires it:

```powershell
.\dist\windows-x64\ps2-driveforge-inspect.exe --physical 3
```

Always verify the Windows disk number before using `--physical`.

## Roadmap

1. **0.1 “Ayanami”** — APA read-only core, diagnostics and initial PFS probing.
2. **0.2 “Bocchi”** — PFS inode/directory/file read path.
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
