# PS2 DriveForge

**Modern read-only APA/PFS HDD management for PlayStation 2.**

Current development train: **0.5.x — “Emilia”**  
Current source version: **0.5.0-dev**

PS2 DriveForge is a Windows-first PS2 HDD management stack built as a safe, testable alternative to shell-oriented `pfsshell` workflows. One native C++20 storage stack is shared by the CLI, host exporter, Win32 GUI, Dokany Explorer provider, benchmark tooling, and the in-progress WinUI 3 frontend.

> **Safety status:** source HDDs/images are read-only. There is no public source write API. Windows `PhysicalDrive` opens with `GENERIC_READ` only. Dokany adds `DOKAN_OPTION_WRITE_PROTECT` and callback-level mutation rejection.

## What Emilia changes

Emilia is the performance and Windows-frontend modernization release. It does not trade format validation for speed.

The HDD-manager contract is deliberately simple:

```text
one validated APA scan
   -> zero-I/O PartitionCatalog
   -> complete management rows immediately
   -> filter / sort / select in memory
   -> optional progressive HDL/PFS enrichment
```

DriveForge does not launch one `HDL.EXE` process per game and does not require probing every PFS filesystem before a management list becomes usable.

The storage path now includes:

- immutable validated PFS probe/node/directory/stat caches;
- bounded 4 KiB small-read cache;
- adaptive sequential read-ahead;
- backing-I/O latency, byte, small-read, cache and concurrency counters;
- explicit-offset `OVERLAPPED` physical/image reads on Windows;
- `pread()` image I/O on POSIX;
- native read-only HDLoader `0xDEADFEED` metadata parsing;
- storage-characteristic hints (`rotational` / `solid-state` / `unknown`) without assuming every user's bridge/HDD behaves like the validation machine.

The first complete real-HDD sweep is preserved in [`docs/emilia-benchmark-2026-08-22.md`](docs/emilia-benchmark-2026-08-22.md). On that specific disk the complete 190-row partition catalog builds in a median **0.006 ms after the APA scan and performs zero additional device I/O**. The result is evidence for the architecture, not a universal speed claim.

## What works now

- disk-image inspection on portable core/host builds;
- read-only Windows `PhysicalDriveN` backend;
- SetupAPI enumeration of actual disk interfaces followed by APA classification;
- APA v2 MBR/checksum/link traversal, main/sub relationships and bounds diagnostics;
- MBR, PFS, HDL and free-space partition classification;
- PFS v3 superblock probing/backup comparison;
- SEGD inode and SEGI indirect descriptor traversal;
- PFS directory/path resolution and arbitrary byte-range reads;
- recursive host export with Windows-safe naming/collision policy;
- zero-I/O `PartitionCatalog` and frontend-neutral `ManagementModel`;
- native scheduled/cancellable HDL metadata enrichment;
- Win32 GUI with System/Light/Dark themes;
- controlled UAC relaunch for raw-disk workflows with non-admin image fallback;
- automatic PS2 HDD discovery/rescan and one-candidate auto-open;
- direct GUI read-only mount / Explorer open / clean unmount;
- standalone `PS2-DriveForge-Mount.exe` diagnostics frontend;
- Dokany 2.3.1 read-only `Partitions\...` namespace;
- `ps2-driveforge-inspect` and `ps2-driveforge-benchmark` tooling;
- WinUI 3 / C++/WinRT frontend project using the same native host/session model;
- generated APA/PFS image E2E with exported-content SHA-256 validation;
- deterministic corruption tests and optional APA libFuzzer target;
- **14 normal regression executables**;
- Windows/MSVC + Dokany/WinUI CI and Linux Clang ASan+UBSan+`-Werror` CI;
- deterministic verification of the canonical Windows release staging tree.

Ayanami, Bocchi, Chisato and Darkness have real-hardware validation. Emilia has real-HDD performance measurements; its final 0.5 release still requires the exact RC artifact to pass the documented Windows/Dokany/Explorer hardware checklist.

## Windows package

During WinUI parity work the canonical Windows package intentionally carries both frontends:

```text
PS2-DriveForge.exe                validated Win32 frontend / current normal entrypoint
WinUI\PS2-DriveForge-WinUI.exe    modern frontend under parity validation
PS2-DriveForge-Mount.exe          thin Dokany diagnostics/script frontend
ps2-driveforge-inspect.exe        inspection CLI
ps2-driveforge-benchmark.exe      benchmark/diagnostics CLI
```

WinUI is **not** a second filesystem implementation. It consumes the same `DriveSession`, `PartitionCatalog`, `ManagementModel`, discovery and enrichment services. It becomes the default only after feature parity and real-hardware validation; 0.5 does not need to fake that milestone by removing the proven Win32 fallback early.

The long-term raw-disk architecture is a normal-user WinUI shell plus a narrow elevated read-only broker. Disk-image workflows must never require elevation merely because the UI is WinUI.

## Build

Full Windows release candidate build:

```powershell
.\build-windows.ps1 `
  -Configuration Release `
  -Clean `
  -WithDokany `
  -DokanyRoot 'C:\Program Files\Dokan\DokanLibrary-2.3.1'

.\scripts\verify-windows-package.ps1
```

See [`BUILDING.md`](BUILDING.md) for prerequisites, WinUI package restore, developer variants, Linux builds and sanitizer/fuzzer configurations.

Portable Linux build/test:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPS2DF_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Performance benchmark

Examples against a physical disk identified for the current Windows session:

```powershell
.\ps2-driveforge-benchmark.exe --physical 3
.\ps2-driveforge-benchmark.exe --physical 3 --browse +OPL /
.\ps2-driveforge-benchmark.exe --physical 3 --hdl
```

`PhysicalDrive3` above is only an example. Windows can renumber physical disks after reboot/reconnection; the GUI discovery path must remain the normal user-facing way to identify the PS2 HDD.

The benchmark reports cold APA/catalog behavior, optional PFS browse/stat cold-vs-warm behavior, backing reads/bytes/service time, max reads in flight, cache/read-ahead counters and native HDL enrichment. `--hdl-qd N` is a developer benchmark override, not a recommendation to force a particular queue depth on every PS2 HDD.

## Architecture at a glance

```text
Win32 GUI        WinUI 3          CLI / benchmark       Dokany
    \              |                   |                  /
     +-------------+-------- ps2driveforge_host --------+
                              |       |       |
                         DriveSession |  ManagementModel
                              |   PartitionCatalog
                              |
                      ps2driveforge_core
                         APA / PFS / HDL
                              |
                 cache / read-ahead / instrumentation
                              |
                    image file / PhysicalDrive
```

Windows device discovery, elevation, theme behavior and Dokany remain above the portable format layers. PFS physical extent translation goes through `ApaVolume`; frontends do not reproduce APA/PFS parsing.

See [`docs/architecture.md`](docs/architecture.md) for dependency rules and layer ownership.

## Current limitations

- source HDD/image mutation is intentionally not implemented;
- writable HDD management requires a future explicit capability with metadata backup/recovery and destructive disposable-image tests;
- the current physical validation HDD does not provide a real large/fragmented PFS SEGI case, so generated-image SEGI/main-sub coverage remains the deterministic guard;
- wider HDD/SSD/USB-bridge samples may justify additional measured tuning, but `unknown` remains a supported normal storage profile;
- WinUI currently has the native session/catalog/enrichment bridge and visual shell, but full image/PFS/export/mount workflows and the least-privilege raw-disk broker have not reached legacy parity;
- HDL virtual ISO browsing/import/export remains later roadmap work;
- project licensing remains TBD pending the upstream-definition/code audit.

## Developer documentation

- [`BUILDING.md`](BUILDING.md) — reproducible Windows/Linux builds and package verification
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — safety, tests, evidence and change expectations
- [`docs/architecture.md`](docs/architecture.md) — current dependency/layer model
- [`docs/testing.md`](docs/testing.md) — all 14 regressions, CI and benchmark test shapes
- [`docs/release-process.md`](docs/release-process.md) — RC/final release gates and rollback evidence
- [`docs/rc-hardware-checklist.md`](docs/rc-hardware-checklist.md) — exact Windows + real-PS2-HDD validation sequence
- [`docs/emilia-plan.md`](docs/emilia-plan.md) — Emilia goals/workstreams
- [`docs/emilia-benchmark-2026-08-22.md`](docs/emilia-benchmark-2026-08-22.md) — measured real-HDD baseline
- [`docs/performance.md`](docs/performance.md) — instrumentation and benchmarking policy
- [`docs/apa-format-notes.md`](docs/apa-format-notes.md) / [`docs/pfs-format-notes.md`](docs/pfs-format-notes.md) — format/addressing notes
- [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md) — preserved hardware evidence and coverage gaps

## Release rule

Do not call a candidate 0.5.0 merely because CI produced a ZIP. Final release requires green deterministic CI, canonical package verification, and the exact candidate artifact passing [`docs/rc-hardware-checklist.md`](docs/rc-hardware-checklist.md) on a real PS2 HDD/Windows/Dokany/Explorer setup.
