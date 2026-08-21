# PS2 DriveForge

**Modern APA/PFS HDD management for PlayStation 2.**

Current development train: **0.5.x — “Emilia”**  
Current source version: **0.5.0-dev**

PS2 DriveForge is a Windows-first PS2 HDD management stack built as a safe, testable alternative to the old shell-oriented workflow around `pfsshell`. One read-only parser/reader stack is shared by the native GUI, CLI, host exporter, Dokany Explorer provider, and Emilia benchmark tools.

> **Safety status:** source devices remain read-only. There is no public HDD write API. `PhysicalDrive` requests `GENERIC_READ` only. Dokany uses `DOKAN_OPTION_WRITE_PROTECT` plus callback-level mutation rejection.

## Why another APA/PFS tool?

`pfsshell` remains useful as a compatibility/reference implementation. DriveForge deliberately avoids its interactive selected-device/current-mount/current-directory shell model and does not shell out for ordinary GUI navigation.

Emilia also targets a second practical pain point: HDD-manager initialization must not walk/probe every partition just to populate a list. The complete management catalog is derived from the already validated APA scan with **zero additional device I/O**; optional PFS/HDL details can be enriched lazily afterwards.

Current architectural differences include:

- byte-addressed `BlockDevice` instead of exposing iomanX semantics to frontends;
- APA main/sub translation isolated in `ApaVolume`;
- explicit PFS path/range reads;
- reusable `DriveSession` shared by CLI, GUI, export, mount, and benchmark code;
- immutable PFS/session metadata caches for read-only sources;
- bounded 4 KiB small-read cache;
- adaptive sequential read-ahead;
- offset-based physical/image I/O instead of a shared seek pointer;
- a zero-I/O `PartitionCatalog` for fast HDD-manager first paint;
- backing-I/O latency/concurrency instrumentation for measured optimization.

DriveForge does not publish throughput wins without measurements. See [`docs/performance.md`](docs/performance.md) and [`docs/emilia-plan.md`](docs/emilia-plan.md).

## What works now

- image-backed PS2 HDD inspection on Windows/Linux/macOS;
- read-only Windows `PhysicalDriveN` backend;
- Windows SetupAPI enumeration of real disk interfaces followed by DriveForge APA classification;
- APA v2 MBR/checksum/link traversal and diagnostics;
- bounds checking for main/sub extents;
- MBR, PFS, HDL, and free partition recognition;
- zero-I/O partition catalog built directly from the APA scan;
- PFS v3 superblock probing and backup comparison;
- SEGD inode reads and SEGI indirect descriptor chains;
- PFS directories/path resolution/random byte-range reads;
- recursive host export with Windows-safe filename handling;
- native Win32 GUI with System/Light/Dark themes;
- automatic raw-disk elevation workflow with limited non-admin fallback;
- automatic startup PS2-HDD discovery and rescan;
- direct GUI read-only mount/open-in-Explorer/unmount commands;
- standalone `PS2-DriveForge-Mount.exe` retained for scripts/debugging;
- Dokany 2.3.1 read-only `Partitions\...` namespace;
- immutable probe/node/stat/directory caches;
- 4 KiB bounded raw read-window cache;
- adaptive large sequential read-ahead;
- Windows physical-disk reads through `FILE_FLAG_OVERLAPPED` with explicit offsets;
- Windows image reads through OVERLAPPED offsets and POSIX image reads through `pread()`;
- `ps2-driveforge-benchmark` cold/warm performance harness;
- generated APA/PFS image E2E with SHA-256 verification;
- deterministic corruption corpus and optional APA libFuzzer target;
- MSVC Windows CI and Clang ASan+UBSan+`-Werror` CI.

Ayanami, Bocchi, Chisato, and Darkness are hardware validated. Darkness validated the complete integrated GUI discovery/elevation/read-only mount workflow on the real 149.05 GiB APA v2 test HDD. Emilia performance measurements are the active development work.

## Emilia performance pipeline

Current data path:

```text
APA / PFS
   -> 4 KiB small-read cache
   -> adaptive sequential read-ahead
   -> backing I/O instrumentation
   -> FileBlockDevice / PhysicalDrive
```

Backing counters sit below both cache layers, so a cache hit genuinely disappears from reported physical/image read calls rather than merely being relabeled.

Session-level caches additionally retain validated PFS probes, resolved nodes/inodes, directory listings, and stat results. `reset_stats()` keeps caches warm for warm-workload measurements; `clear_caches()` provides an explicit cold-cache boundary.

The benchmark frontend can be run read-only against an image or physical disk:

```powershell
.\ps2-driveforge-benchmark.exe --physical 3
.\ps2-driveforge-benchmark.exe --physical 3 --browse +OPL /
```

It reports APA scan time, zero-I/O catalog build time, cold/warm metadata workloads, backing calls/bytes/service time, max I/O concurrency, cache hit/miss data, and read-ahead activity.

## HDD Manager direction

The management UI model is intentionally split into two phases:

```text
one APA scan
   -> PartitionCatalog
   -> complete rows immediately
   -> UI first paint
   -> optional lazy/batched metadata enrichment
```

Sorting, filtering, showing/hiding sub-partitions, and selecting rows are memory-only operations. The first usable list must not wait for every PFS filesystem to be probed or for every HDL game title to be resolved.

## Windows GUI

`PS2-DriveForge.exe` remains the normal Windows frontend while the WinUI 3 replacement is developed in parallel.

Current Win32 workflow supports:

- `Open disk image...`;
- automatic PS2 HDD detection and selection;
- APA/PFS browsing;
- file export;
- `Mount read-only...`;
- `Open mounted volume in Explorer`;
- `Unmount`;
- `View -> Theme -> System / Light / Dark`.

The old visible `PhysicalDrive0..31` list is gone. SetupAPI discovers actual disk interfaces, DriveForge validates APA, and one candidate can auto-open.

## WinUI 3 direction

Emilia is also building a modern WinUI 3 frontend. Storage, APA/PFS, discovery, catalog, performance, and Dokany code stay outside XAML/Windows App SDK.

The long-term elevation model is:

```text
WinUI 3 shell (normal user)
        |
 controlled read-only IPC
        |
elevated raw-disk broker
        |
GENERIC_READ PhysicalDrive
```

This keeps the user-facing UI out of an elevated process and limits administrator privileges to the smallest component that actually needs raw disk access. The legacy Win32 frontend remains a fallback until WinUI reaches feature and hardware parity.

## Current limitations

- source HDD/image mutation is not implemented;
- real-HDD PFS SEGI/large-fragmented-file traversal has not been observed because the current test HDD stores large content as HDL; generated-image SEGI/main-sub coverage is green;
- adaptive cache/read-ahead tuning still needs real-HDD benchmark data;
- queue-depth tuning and HDL title enrichment are active Emilia work;
- WinUI 3 frontend/broker implementation is not yet feature-complete;
- HDL virtual ISO browsing/import/export remains later roadmap work;
- project licensing remains TBD until the upstream-definition/code audit is complete.
