# Performance notes and benchmark plan

DriveForge is intended to become substantially more convenient and, where the host/device allows it, faster than legacy shell-oriented workflows. This document exists to prevent us from confusing a cleaner architecture with measured performance.

## Current read path

For a physical Windows disk today:

```text
GUI / CLI / Dokany
  -> DriveSession
    -> pfs::Reader::read
      -> read_zone_bytes
        -> ApaVolume::read_sectors
          -> InstrumentedBlockDevice::read
            -> PhysicalDrive::read
              -> mutex
              -> SetFilePointerEx
              -> synchronous ReadFile
```

For an image file the final layer is a mutex-protected `std::ifstream` seek/read.

Current relevant sizes:

- PS2 sector: 512 B;
- PFS metadata record: 1024 B;
- validated real-HDD zone size so far: 8 KiB;
- aligned PFS lower-level read batch: up to 128 sectors = 64 KiB;
- host export request buffer: 1 MiB.

The 1 MiB exporter buffer does **not** imply a single 1 MiB device read. The current PFS layer may split it into multiple 64 KiB lower-level reads plus extent-transition reads.

## Instrumentation implemented

`InstrumentedBlockDevice` measures actual calls reaching the backing `BlockDevice` after PFS batching and APA translation. `DriveSession` tracks high-level operation counts.

Current counters:

- APA scans;
- PFS browse operations;
- PFS export operations;
- backing `read()` calls;
- backing bytes requested;
- failed backing reads;
- largest backing read;
- derived average backing-read size.

CLI exposes them with:

```powershell
ps2-driveforge-inspect.exe --stats --physical 3 --browse +OPL
ps2-driveforge-inspect.exe --stats --physical 3 --extract +OPL / exported-OPL
```

The GUI status bar also exposes cumulative backing reads/bytes for its active `DriveSession`.

These counters are measurement plumbing, not proof that DriveForge is already faster than pfsshell/pfsfuse.

Still missing for Emilia profiling:

- logical reader calls/bytes separated from backend bytes;
- metadata vs payload read classification;
- elapsed-operation timings;
- inode/directory/block cache hit/miss counters;
- coalescing/read-ahead usefulness counters;
- concurrency/queue information after overlapped I/O exists.

## Existing real-HDD baseline

The Chisato `+OPL` browse workload recorded:

```text
APA scans:             1
PFS browse operations: 1
Backing read calls:    215
Backing bytes read:    206.50 KiB
Average backing read:  983 B
Largest backing read:  1.00 KiB
Failed backing reads:  0
```

This already demonstrates a metadata-heavy pattern dominated by roughly 1 KiB backing requests.

## Darkness Explorer baseline

Explorer is now hardware-validated through the Darkness Dokany path and is noticeably more metadata-hungry than the DriveForge GUI/CLI. Before merging 0.4 and changing cache/I/O behavior in 0.5, preserve this final real-machine workload:

```text
cold GUI start
 -> UAC / SetupAPI discovery / auto-open
 -> Mount read-only
 -> Explorer root
 -> Partitions
 -> +OPL
 -> __common\OPL
 -> read/copy conf_hdd.cfg
 -> Unmount
```

For the final Darkness validation preserve, where practical:

- the callback/debug trace for the workload;
- backing-I/O totals visible from the session/tooling;
- the copied file hash;
- whether the run started cold or followed previous browsing.

Emilia should repeat the same navigation/copy sequence before and after optimization classes. Subjective Explorer responsiveness is useful context, but not the benchmark by itself.

## Current strengths

The current API enables optimization without changing format semantics:

- byte-range reads carry explicit offsets;
- PFS logical extents are separate from APA physical extents;
- GUI, CLI, export, and Dokany use the same session/parser path;
- instrumentation is transparent below format parsing;
- mounted reads do not require a process-global current directory;
- validated read-only metadata is suitable for immutable caching in Emilia.

These are enablers, not benchmark results.

## Known bottlenecks

### Global serialization per backing device

`PhysicalDrive` and `FileBlockDevice` currently protect seek/read operations with a mutex. Parallel filesystem callbacks therefore serialize at the backing device.

### No metadata caching

Explorer repeatedly opens/stats/enumerates the same nodes. Inode and directory metadata can currently be reread many times.

### No read-window cache

Repeated or overlapping small reads can cause repeated host I/O for bytes that were just fetched.

### Fixed 64 KiB aligned batch

`read_zone_bytes()` currently caps an aligned lower-level batch at 128 sectors. This was a conservative correctness choice, not a tuned optimum.

### Synchronous Windows API

The physical backend uses `SetFilePointerEx` + synchronous `ReadFile`; no offset/overlapped or queued model exists yet.

## Emilia optimization order

Unless measurements contradict it, 0.5 should proceed in this order:

1. add missing logical/metadata/payload/timing counters;
2. immutable inode metadata cache;
3. directory cache;
4. block/read-window cache;
5. adjacent request coalescing inside validated APA extents;
6. adaptive sequential read-ahead;
7. Windows offset-based/overlapped physical reads;
8. concurrent scheduling where safe;
9. benchmark the preserved Darkness workload and representative export workloads;
10. compare against pfsshell/pfsfuse-equivalent operations only with controlled conditions.

A custom kernel filesystem/storage driver is **not** the performance plan. Dokany remains the Explorer bridge; intended gains are in the userspace data path.

## Benchmark workloads

At minimum keep distinct workloads for:

### A. Large sequential file

Measures sequential throughput, request size/coalescing, and bridge/device behavior.

### B. Many small files

Measures metadata and host-file-creation overhead.

### C. Directory browsing

Repeated enumeration/stat of the same directory measures inode/directory cache value.

### D. Mounted Explorer reads

The preserved Darkness navigation/copy workload measures real Dokany metadata churn, random reads, and mount overhead.

## Comparison rules for pfsshell/pfsfuse

When publishing comparisons, record exact DriveForge and pfsshell versions/commits, physical HDD and bridge/adapter, Windows version, source path/workload, host destination, cold/warm cache state, DriveForge backing-I/O counters, and multiple timed runs where variance matters.

Do not compare a warm DriveForge run with a cold pfsshell run.

## Correctness gates

An optimization is not accepted if it breaks:

- APA chain/extent validation;
- PFS checksum/magic validation;
- arbitrary unaligned reads;
- extent crossing;
- APA main/sub translation;
- SEGI traversal;
- generated-image SHA-256 E2E;
- corruption corpus;
- Dokany read-only semantics;
- physical `GENERIC_READ` invariant;
- sanitizer/MSVC CI;
- recorded hardware cases.

A cache may memoize validated information; it must not become a second, weaker parser.

Fast corruption is not a feature.
