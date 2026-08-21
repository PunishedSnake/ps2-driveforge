# PS2 DriveForge

**Modern APA/PFS HDD management for PlayStation 2.**

Current development train: **0.4.x — “Darkness”**  
Current source version: **0.4.0-dev**

PS2 DriveForge is a Windows-first PS2 HDD management stack. It is being built as a safe, testable alternative to the old shell-oriented workflow around `pfsshell`: one read-only parser/reader stack shared by a native GUI, CLI, host export layer, and Dokany Explorer provider.

> **Safety status:** source devices remain read-only. There is no public HDD write API and the Windows physical-drive backend requests `GENERIC_READ` only. Export writes only to user-selected host-side files/directories. Darkness mounts also request `DOKAN_OPTION_WRITE_PROTECT` and reject mutation callbacks.

## Why another APA/PFS tool?

`pfsshell` remains useful and is one of DriveForge's compatibility references. DriveForge deliberately changes the host-side architecture rather than wrapping the interactive shell model in a prettier frontend.

The important differences today are:

- no process-global selected-device/current-mount/current-directory model in the parser;
- a byte-addressed `BlockDevice` boundary instead of exposing iomanX semantics to frontends;
- APA main/sub-partition translation isolated in `ApaVolume`;
- explicit `PFS Reader + Node + offset + span` byte-range reads;
- a reusable `DriveSession` shared by CLI, GUI and mount-facing host logic for APA scan, PFS browsing, export and diagnostics;
- host filename/export policy isolated above the parser;
- one shared `DokanyMountController` used by the native GUI and the diagnostic mount CLI;
- structural read-only safety while the parser and mount provider are still maturing.

This does **not** mean DriveForge has already benchmarked faster throughput than pfsshell/pfsfuse. Current physical-drive I/O is synchronous and deliberately conservative. DriveForge has backing-I/O counters so later optimization can be measured instead of guessed. See [`docs/performance.md`](docs/performance.md) and [`docs/pfsshell-comparison.md`](docs/pfsshell-comparison.md).

## What works now

- open PS2 HDD images on Windows, Linux and macOS;
- open `\\.\PhysicalDriveN` read-only on Windows;
- enumerate actual Windows disk interfaces through SetupAPI and map them to their real `PhysicalDriveN` identifiers;
- automatically probe real disk devices for PS2 APA at GUI startup rather than guessing `PhysicalDrive0..31`;
- detect and validate the APA v2 MBR;
- enumerate and diagnose APA partition chains;
- reject APA main/sub extents that leave the backing device;
- recognize MBR, PFS, HDL and free partitions;
- account for APA sub-partitions and non-contiguous extents;
- probe PFS primary/backup superblocks and validate zone size;
- read and checksum PFS SEGD inodes;
- follow indirect SEGI descriptor chains for large/fragmented files;
- enumerate PFS directories and resolve paths;
- read arbitrary byte ranges, including cross-sector ranges;
- browse PFS partitions from the CLI and native GUI;
- recursively export PFS files/directories to the host;
- sanitize PS2 filenames for Windows host restrictions;
- detect export directory cycles and host-name collisions;
- collect backing-I/O statistics for browse/export operations;
- native Windows GUI with automatic PS2 HDD discovery, APA partition tree, PFS browser and shared `DriveSession` path;
- controlled UAC relaunch for raw-disk GUI use, with a limited image-capable fallback if elevation is cancelled;
- persistent Windows GUI System/Light/Dark theme selection with High Contrast override;
- read-only Dokany 2.3.1 mount exposing `\\Partitions\\...` through Explorer;
- direct GUI `Mount read-only`, `Open mounted volume in Explorer`, and `Unmount` actions;
- automatic free-drive-letter selection for GUI mounts, preferring `P:`;
- standalone diagnostic/script mount CLI using the same controller/callback implementation as the GUI;
- explicit read-only mount policy for NT `FILE_*` create dispositions;
- optional mount callback tracing through `PS2-DriveForge-Mount.exe --debug`;
- build/test on Windows x64 with MSVC;
- run Clang ASan + UBSan with warnings-as-errors in CI;
- generate a complete APA/PFS `.img` during tests and verify recursive exports by SHA-256;
- run a deterministic malformed-metadata regression corpus;
- optionally build an APA libFuzzer target under Clang.

Ayanami, Bocchi, and Chisato have been validated against a real **149.05 GiB PS2 HDD**. Chisato's read-only discovery isolated `PhysicalDrive3` as the only PS2 APA disk (APA v2, 190 headers), the GUI exposed 43 main partitions with clean diagnostics, recursive `+OPL` export succeeded, and a real regular PFS file was extracted from `__common`. Darkness has now also been hardware-validated through Dokany: Explorer opens the mounted PS2PFS volume, browses `+OPL` and `__common\OPL`, copies `conf_hdd.cfg` with the expected SHA-256, rejects write creation, and unmounts cleanly. The remaining 0.4 hardware gate is the new **integrated GUI** discovery/elevation/mount workflow. See [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md) and [`docs/darkness-plan.md`](docs/darkness-plan.md).

## Current limitations

These are deliberate or known gaps, not hidden TODOs:

- source HDD/image mutation is not implemented;
- the new integrated Darkness GUI discovery/elevation/mount workflow still needs a final real-machine smoke test before 0.4 is merged;
- real-HDD PFS SEGI/large-fragmented-file traversal has not yet been observed because the current test disk stores large content as HDL partitions; deterministic generated-image SEGI coverage is green;
- Windows physical-drive reads are currently synchronous and serialized per device;
- no inode/directory/block cache exists yet;
- no read-ahead or overlapped I/O exists yet;
- current instrumentation measures backing reads, but not yet metadata-vs-payload/cache counters;
- HDL virtual ISO browsing/import/export is a later milestone;
- project licensing remains intentionally TBD until the upstream-code/definition audit is complete.

## Windows GUI

The Windows package contains `PS2-DriveForge.exe` in addition to the CLI inspector and the diagnostic Darkness mount frontend.

Current GUI behavior:

- when started without an elevated token, requests a controlled UAC `runas` relaunch; cancelling UAC leaves DriveForge usable for disk images and provides `Restart as Administrator` in the File menu;
- enumerates real Windows disk interfaces through SetupAPI at startup;
- maps each interface to the actual backing `PhysicalDriveN` with `IOCTL_STORAGE_GET_DEVICE_NUMBER` and probes it read-only with the normal APA parser;
- if exactly one PS2 APA HDD is found, opens it automatically;
- `File -> PS2 HDDs` lists only detected PS2 APA candidates, showing useful device metadata instead of a hard-coded `PhysicalDrive0..31` list;
- `File -> PS2 HDDs -> Rescan PS2 HDDs` repeats discovery after hot-plug/adapter changes;
- `File -> Open disk image...` opens a raw PS2 HDD image without depending on physical-disk discovery;
- `File -> Mount read-only...` mounts the currently open image/HDD through the shared Dokany controller using `P:` when available or another free drive letter;
- `File -> Open mounted volume in Explorer` opens the mounted DriveForge volume with the Windows shell;
- `File -> Unmount` removes the Dokany mount cleanly;
- source switching is blocked while a DriveForge volume remains mounted;
- `View -> Theme -> System / Light / Dark` changes the native GUI theme dynamically and persists the preference;
- System follows Windows application theme preference while High Contrast takes precedence;
- the left pane lists APA main partitions;
- selecting a PFS partition opens its root directory in the right pane;
- double-click a folder to navigate into it;
- use `..` to navigate upward;
- double-click a regular file to export it with a normal Windows **Save As** dialog;
- status text identifies the current PFS path, read-only state, backing reads, discovery state and mount state.

The GUI does not construct its own PFS stack or duplicate Dokany callbacks. GUI, CLI and the Explorer provider share the same host/session/mount layers. MSVC builds the Win32 UI target with an explicit UTF-8 source/exec character set so non-ASCII UI punctuation is rendered correctly.

## Windows disk discovery

The old development-only `PhysicalDrive0..31` scan has been replaced in Darkness.

Current discovery path:

```text
GUID_DEVINTERFACE_DISK
  -> SetupDiEnumDeviceInterfaces
  -> device-interface path
  -> IOCTL_STORAGE_GET_DEVICE_NUMBER
  -> actual PhysicalDriveN
  -> DriveForge PhysicalDrive (GENERIC_READ)
  -> APA parser/probe
```

The SetupAPI interface handle is opened without disk-data access and is used only to obtain the Windows storage-device number. PS2 identity is determined by DriveForge's APA parser, not by model name, capacity, drive letter, GPT/MBR assumptions or a guessed numeric range.

## Darkness Dokany mount

The preferred interactive path is now the native GUI. `PS2-DriveForge-Mount.exe` remains intentionally available for scripts, automation and debugging; it is a thin frontend over the same `DokanyMountController` linked by the GUI.

Dokany 2.x runtime/driver must be installed on the Windows machine that performs the mount.

Diagnostic CLI examples:

```powershell
.\PS2-DriveForge-Mount.exe --physical 3 --mount P:
.\PS2-DriveForge-Mount.exe --image disk.img --mount P:
.\PS2-DriveForge-Mount.exe --unmount P:
.\PS2-DriveForge-Mount.exe --physical 3 --mount P: --debug
```

The mounted namespace begins at:

```text
P:\
  Partitions\
    <PFS partition>\
      ...
```

The mount is structurally read-only: `PhysicalDrive` requests `GENERIC_READ`, no `BlockDevice::write()` API exists, Dokany uses `DOKAN_OPTION_WRITE_PROTECT`, and create/write/delete/rename/truncate/attribute-mutation requests are rejected.

A subtle Dokany rule is documented and regression-tested: the `ZwCreateFile` callback receives NT kernel `FILE_OPEN / FILE_CREATE / FILE_OPEN_IF / ...` dispositions, not Win32 `OPEN_EXISTING / CREATE_NEW / OPEN_ALWAYS / ...`. Several values overlap numerically while meaning different things; confusing them caused the first real Darkness mount to return `STATUS_OBJECT_NAME_COLLISION` for a normal root open (`FILE_OPEN == 1`).

## Windows x64 build

Install Visual Studio 2022 with **Desktop development with C++**, including MSVC, Windows 10/11 SDK and CMake tools. For the Dokany mount target, install the Dokany 2.x development SDK/runtime. In Developer PowerShell:

```powershell
.\build-windows.ps1 -Clean -WithDokany -DokanyRoot 'C:\Program Files\Dokan\DokanLibrary-2.3.1'
```

Output is placed in `dist\windows-x64` and packaged as:

```text
PS2-DriveForge-0.4.0-Darkness-Release-windows-x64.zip
```

GitHub Actions performs the same MSVC build automatically for pull requests and `main`. Darkness development packages include the GUI, CLI, mount frontend, all eight regression-test executables, and the current testing/hardware-validation notes.

## Generic CMake build

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## CLI

### Detect PS2 HDD candidates on Windows

```powershell
.\ps2-driveforge-inspect.exe --detect-physical
```

The CLI uses the same Windows disk-interface enumeration/probe layer as the GUI and reports size, APA detection/version and header count. It never selects or enables a write target.

### Inspect an image

```bash
./build/ps2-driveforge-inspect disk.img
```

### Inspect a physical drive on Windows

Run an elevated terminal if Windows requires it. Detection is preferable to guessing the disk number:

```powershell
.\ps2-driveforge-inspect.exe --detect-physical
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

### Collect backing-I/O statistics

Put `--stats` before the source selector:

```powershell
.\ps2-driveforge-inspect.exe --stats --physical 3 --browse +OPL
.\ps2-driveforge-inspect.exe --stats --physical 3 --extract +OPL / exported-OPL
```

Current output includes APA scan count, browse/export operation counts, backing `read()` calls, bytes read, average/largest backing read and failures. These numbers establish a baseline for future Emilia optimization; they are not a performance claim by themselves.

The first real-HDD `+OPL` browse baseline recorded under Chisato was **215 backing reads / 206.50 KiB**, with a 983 B average, 1 KiB largest read, and zero failed reads.

## Test coverage

The normal CTest suite currently contains **8 test executables** and is run under both Windows/MSVC and Clang ASan+UBSan.

Coverage includes:

- valid/invalid APA chains, checksum and cycle detection;
- APA main/sub extent bounds validation;
- PFS primary/backup superblocks;
- invalid PFS zone and missing-subpart rejection;
- root inode and directory enumeration;
- path resolution;
- bad inode checksum and malformed dentry rejection;
- unaligned reads crossing a 512-byte sector boundary;
- indirect SEGI descriptor traversal;
- Windows-safe host filename conversion;
- `DriveSession` scan/browse/export and I/O counters;
- generated real `.img` end-to-end traversal through `FileBlockDevice`;
- nested directories and empty directories;
- a file crossing APA main/sub-partition extents;
- generated SEGI-backed file export;
- recursive host export integrity verified by SHA-256;
- Dokany read-only create/open disposition policy, including the `FILE_OPEN == 1` root-open regression.

An optional Clang/libFuzzer APA target is also available. See [`docs/testing.md`](docs/testing.md) for the exact fixture, corruption corpus, fuzz commands, statistics, and hardware-test workflow.

## Developer documentation

Start here when changing core behavior rather than guessing from old chat/commit history:

| Document | Purpose |
| --- | --- |
| [`docs/architecture.md`](docs/architecture.md) | Layer boundaries and dependency direction |
| [`docs/apa-format-notes.md`](docs/apa-format-notes.md) | APA units, links, checksums and main/sub extents |
| [`docs/pfs-format-notes.md`](docs/pfs-format-notes.md) | PFS zones, metadata, SEGD/SEGI and dentry traps |
| [`docs/pfsshell-comparison.md`](docs/pfsshell-comparison.md) | What we changed relative to the pfsshell architecture |
| [`docs/performance.md`](docs/performance.md) | Current bottlenecks, implemented instrumentation and benchmark plan |
| [`docs/testing.md`](docs/testing.md) | E2E image fixture, corruption corpus, fuzzing and hardware-test workflow |
| [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md) | What has actually worked on a physical PS2 HDD |
| [`docs/development-guidelines.md`](docs/development-guidelines.md) | Documentation/comment/testing definition of done |
| [`docs/release-codenames.md`](docs/release-codenames.md) | The regrettably permanent anime release train |

## Roadmap

1. **0.1 “Ayanami”** — APA read-only core, diagnostics and initial PFS probing. **Done.**
2. **0.2 “Bocchi”** — PFS inode/directory/file read path. **Done / hardware validated.**
3. **0.3 “Chisato”** — native Windows GUI browser, reusable host/session layer, discovery, diagnostics and pre-hardware hardening. **Done / hardware validated.**
4. **0.4 “Darkness”** — read-only Explorer mount plus integrated Windows discovery/elevation/mount workflow. **In development / core mount hardware validated.**
5. **0.5 “Emilia”** — cache, read-ahead and overlapped-I/O performance pass.
6. **0.6 “Frieren”** — carefully gated PFS/APA write path with automatic metadata backup.
7. **0.7 “Guts”** — HDL game view and virtual ISO import/export.
8. **0.8 “Holo”** — PS2 MBR/boot manager/recovery integration.

Release train names proceed alphabetically by anime character. Patch releases inherit the codename of their minor release.

## Architecture

```text
 Native Win32 GUI        inspector CLI        mount CLI
        |                     |                  |
        |                     |         DokanyMountController
        |                     |                  |
        +--------------- DriveSession / host ----+
                              |
                   ps2driveforge_core
                              |
                +-------------+-------------+
                |             |             |
               APA           PFS         HDL/MBR
                |             |             |
                +-------------+-------------+
                              |
                  InstrumentedBlockDevice
                              |
                        BlockDevice
                      /             \
                disk image      PhysicalDriveN
```

Format code does not know about Win32 controls, Windows filenames, SetupAPI, UAC or Dokany. Frontends do not implement APA/PFS parsing. `InstrumentedBlockDevice` is a transparent diagnostic wrapper, not a new on-disk abstraction.

## Format references

APA/PFS layout and behavior are checked against current PS2SDK `libapa`, `libpfs` and related headers. `pfsshell` remains a compatibility reference, but DriveForge deliberately does not adopt its stateful shell/iomanX frontend architecture.

Upstream references:

- <https://github.com/ps2dev/ps2sdk>
- <https://github.com/ps2homebrew/pfsshell>

## License

**TBD during the private prototype phase.** Before public release we will audit retained upstream definitions/code and choose a compatible project license.
