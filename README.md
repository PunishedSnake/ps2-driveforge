# PS2 DriveForge

**Modern APA/PFS HDD management for PlayStation 2.**

Current development train: **0.4.x — “Darkness”**  
Current source version: **0.4.0-dev**

PS2 DriveForge is a Windows-first PS2 HDD management stack built as a safe, testable alternative to the old shell-oriented workflow around `pfsshell`. One read-only parser/reader stack is shared by the native GUI, CLI, host exporter, and Dokany Explorer provider.

> **Safety status:** source devices remain read-only. There is no public HDD write API. `PhysicalDrive` requests `GENERIC_READ` only. Darkness additionally uses `DOKAN_OPTION_WRITE_PROTECT` plus callback-level mutation rejection.

## Why another APA/PFS tool?

`pfsshell` remains useful and is a compatibility/reference implementation. DriveForge deliberately changes the host architecture rather than wrapping an interactive selected-device/current-mount/current-directory shell model.

Current differences include:

- byte-addressed `BlockDevice` instead of exposing iomanX semantics to frontends;
- APA main/sub translation isolated in `ApaVolume`;
- explicit PFS path/range reads;
- reusable `DriveSession` shared by CLI, GUI, export, and mount-facing code;
- host filename/export policy above the parser;
- a portable `ReadOnlyMountView` above `DriveSession`;
- structural read-only safety;
- backing-I/O instrumentation for measured optimization work.

DriveForge does **not** yet claim higher throughput than pfsshell/pfsfuse. Current raw-disk I/O is intentionally synchronous and conservative. See [`docs/performance.md`](docs/performance.md) and [`docs/pfsshell-comparison.md`](docs/pfsshell-comparison.md).

## What works now

- image-backed PS2 HDD inspection on Windows/Linux/macOS;
- read-only Windows `PhysicalDriveN` backend;
- Windows SetupAPI enumeration of actual disk interfaces followed by DriveForge APA classification;
- APA v2 MBR/checksum/link traversal and diagnostics;
- bounds checking for main/sub extents;
- MBR, PFS, HDL, and free partition recognition;
- PFS v3 superblock probing and backup comparison;
- SEGD inode reads and SEGI indirect descriptor chains;
- PFS directories/path resolution/random byte-range reads;
- recursive host export with Windows-safe filename handling;
- backing-I/O counters;
- native Win32 GUI with System/Light/Dark themes;
- automatic raw-disk elevation workflow with limited non-admin fallback;
- automatic startup PS2-HDD discovery and rescan;
- direct GUI read-only mount/open-in-Explorer/unmount commands;
- standalone `PS2-DriveForge-Mount.exe` retained for scripts/debugging;
- Dokany 2.3.1 read-only `Partitions\...` namespace;
- generated APA/PFS image E2E with SHA-256 verification;
- deterministic corruption corpus and optional APA libFuzzer target;
- MSVC Windows CI and Clang ASan+UBSan+`-Werror` CI.

Ayanami, Bocchi, and Chisato are hardware validated. Darkness' standalone/shared-controller Dokany path is also hardware validated on the real test HDD: Explorer browsing, mounted file copy/hash integrity, write rejection, and clean unmount all passed. The remaining 0.4 gate is the final integrated GUI discovery/elevation/mount workflow. See [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md).

## Current limitations

- source HDD/image mutation is not implemented;
- the final Darkness integrated-GUI hardware smoke test is still pending before merge;
- real-HDD PFS SEGI/large-fragmented-file traversal has not been observed because the current test HDD stores large content as HDL; generated-image SEGI/main-sub coverage is green;
- physical-drive reads are synchronous and serialized per device;
- no inode/directory/block cache yet;
- no read-ahead/request coalescing/overlapped raw I/O yet;
- HDL virtual ISO browsing/import/export is later work;
- project licensing remains TBD until the upstream-definition/code audit is complete.

## Windows GUI

`PS2-DriveForge.exe` is the normal Windows frontend.

On startup:

```text
normal user
   -> controlled UAC runas relaunch (when accepted)
   -> SetupAPI disk-interface enumeration
   -> IOCTL_STORAGE_GET_DEVICE_NUMBER
   -> read-only PhysicalDrive open
   -> DriveForge APA probe
   -> one PS2 candidate auto-opens
```

Cancelling UAC leaves DriveForge running in a limited mode so disk images remain usable. `File -> Restart as Administrator` is available later.

The old visible `PhysicalDrive0..31` list is gone. `File -> PS2 HDDs` shows only detected PS2 APA candidates with friendly name, size, APA version, and main-partition count. `Rescan PS2 HDDs` repeats discovery without silently replacing an already-open source.

Current GUI operations:

- `Open disk image...`;
- select a detected PS2 HDD;
- browse APA/PFS;
- double-click files for normal host export;
- `Mount read-only...`;
- `Open mounted volume in Explorer`;
- `Unmount`;
- `View -> Theme -> System / Light / Dark`.

Mounting prefers `P:` when free and falls back to another unused data-drive letter. The GUI and standalone mount CLI use the same `DokanyMountController`; the GUI does **not** launch the console mount program as a helper subprocess.

## Darkness Dokany mount

Darkness exposes:

```text
<mount>:\
  Partitions\
    <PFS partition>\
      <PFS tree>
```

`Games`/HDL and synthetic MBR/recovery views are later milestones.

The standalone diagnostic frontend remains available:

```powershell
.\PS2-DriveForge-Mount.exe --physical 3 --mount P:
.\PS2-DriveForge-Mount.exe --image disk.img --mount P:
.\PS2-DriveForge-Mount.exe --physical 3 --mount P: --debug
.\PS2-DriveForge-Mount.exe --unmount P:
```

The first real mount exposed an important Dokany contract bug: `ZwCreateFile` receives NT `FILE_OPEN / FILE_CREATE / FILE_OPEN_IF / ...` disposition values, not Win32 `OPEN_EXISTING / CREATE_NEW / ...`. `FILE_OPEN == 1` was initially mistaken for `CREATE_NEW == 1`, which caused Explorer's root open to return `The file exists.` The corrected policy is regression-tested on every CI platform.

The corrected physical-HDD mount is hardware validated: `P:\` opened normally, `Partitions\+OPL` and `Partitions\__common\OPL` enumerated, Explorer copied `conf_hdd.cfg` with SHA-256 `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`, write creation was rejected, and unmount completed cleanly.

## Windows x64 build

Install Visual Studio 2022 with Desktop development with C++, a Windows SDK, and CMake. Dokany mount builds additionally need the Dokany 2.x development SDK/runtime.

```powershell
.\build-windows.ps1 -Clean -WithDokany -DokanyRoot 'C:\Program Files\Dokan\DokanLibrary-2.3.1'
```

Output package:

```text
PS2-DriveForge-0.4.0-Darkness-Release-windows-x64.zip
```

Windows CI pins the official Dokany 2.3.1 x64 MSI, verifies its SHA-256, installs development files, builds/tests/packages the GUI, inspector, mount CLI, and regression executables.

## Generic CMake build

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## CLI examples

Detect PS2 HDD candidates on Windows:

```powershell
.\ps2-driveforge-inspect.exe --detect-physical
```

Inspect:

```powershell
.\ps2-driveforge-inspect.exe disk.img
.\ps2-driveforge-inspect.exe --physical 3
```

Browse:

```powershell
.\ps2-driveforge-inspect.exe --physical 3 --browse +OPL
.\ps2-driveforge-inspect.exe --physical 3 --browse +OPL CFG
```

Export:

```powershell
.\ps2-driveforge-inspect.exe --physical 3 --extract +OPL / exported-OPL
```

Collect backing-I/O statistics:

```powershell
.\ps2-driveforge-inspect.exe --stats --physical 3 --browse +OPL
```

The first real-HDD `+OPL` browse baseline is **215 backing reads / 206.50 KiB**, average 983 B, largest 1 KiB, zero failed reads. Darkness' final Explorer workload will be preserved as the second baseline before Emilia optimization.

## Test coverage

The normal CTest suite contains **9 executables**:

```text
ps2-driveforge-tests
ps2-driveforge-pfs-file-tests
ps2-driveforge-pfs-segi-tests
ps2-driveforge-host-tests
ps2-driveforge-e2e-image-tests
ps2-driveforge-corruption-tests
ps2-driveforge-session-tests
ps2-driveforge-dokany-open-policy-tests
ps2-driveforge-darkness-policy-tests
```

The Darkness policy test records the required one-candidate auto-open and deterministic free-drive-letter behavior independently from live SetupAPI/Dokany state, so CI can catch policy drift without depending on a particular runner's disks or letters. See [`docs/testing.md`](docs/testing.md) for exact coverage and the final hardware workflow.

## Developer documentation

| Document | Purpose |
| --- | --- |
| [`docs/architecture.md`](docs/architecture.md) | Layer boundaries and dependency direction |
| [`docs/apa-format-notes.md`](docs/apa-format-notes.md) | APA units, links, checksums, extents |
| [`docs/pfs-format-notes.md`](docs/pfs-format-notes.md) | PFS zones, metadata, SEGD/SEGI, dentries |
| [`docs/pfsshell-comparison.md`](docs/pfsshell-comparison.md) | Architecture comparison with pfsshell |
| [`docs/performance.md`](docs/performance.md) | Bottlenecks, instrumentation, benchmark plan |
| [`docs/testing.md`](docs/testing.md) | E2E/corruption/fuzz/hardware test workflow |
| [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md) | Proven physical-HDD behavior and gaps |
| [`docs/darkness-plan.md`](docs/darkness-plan.md) | 0.4 scope and final hardware gate |
| [`docs/development-guidelines.md`](docs/development-guidelines.md) | Definition of done for code/docs/tests |
| [`docs/release-codenames.md`](docs/release-codenames.md) | Release train |

## Roadmap

1. **0.1 “Ayanami”** — APA read-only core. **Done.**
2. **0.2 “Bocchi”** — PFS read path. **Done / hardware validated.**
3. **0.3 “Chisato”** — Windows GUI/shared host/session/export hardening. **Done / hardware validated.**
4. **0.4 “Darkness”** — read-only Dokany Explorer mount and integrated Windows device workflow. **In development / final GUI hardware gate pending.**
5. **0.5 “Emilia”** — cache, read-ahead, request coalescing, overlapped-I/O performance pass.
6. **0.6 “Frieren”** — gated PFS/APA write path with automatic metadata backup.
7. **0.7 “Guts”** — HDL game view and virtual ISO import/export.
8. **0.8 “Holo”** — PS2 MBR/boot manager/recovery integration.

## Architecture

```text
 Native Win32 GUI       CLI / mount CLI       Dokany callbacks
        |                      |                      |
        +----------- shared host/session/mount ------+
                               |
                         DriveSession
                               |
                    ps2driveforge_core
                               |
                         APA / PFS
                               |
                    InstrumentedBlockDevice
                               |
                    image / PhysicalDrive
```

Format code does not know about Win32 controls, Windows filenames, SetupAPI, UAC, or Dokany. Frontends do not implement APA/PFS parsing.

## Format references

APA/PFS layout is checked against current PS2SDK `libapa`, `libpfs`, and related definitions. `pfsshell` remains a compatibility reference.

- <https://github.com/ps2dev/ps2sdk>
- <https://github.com/ps2homebrew/pfsshell>

## License

**TBD during the private prototype phase.** Before public release we will audit retained upstream definitions/code and select a compatible project license.
