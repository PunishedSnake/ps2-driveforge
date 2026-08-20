# Performance notes and benchmark plan

DriveForge is intended to become substantially more convenient and, where the host/device allows it, faster than legacy shell-oriented workflows. This document exists to prevent us from confusing a cleaner architecture with measured performance.

## Current read path

For a physical Windows disk today:

```text
GUI / CLI
  -> pfs::Reader::read
    -> read_zone_bytes
      -> ApaVolume::read_sectors
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

The 1 MiB exporter buffer does **not** mean one 1 MiB device read. The current PFS layer can split it into multiple 64 KiB lower-level reads and additional reads when the file crosses PFS extents.

## Current strengths

Even before a cache exists, the current API enables optimizations that would be awkward if every frontend had to reproduce a selected-device/current-mount/current-directory model:

- byte-range reads carry explicit offsets;
- PFS logical extents are already separated from APA physical extents;
- the caller can keep a `Reader`/`Node` and issue repeated reads without re-resolving a shell path;
- host export can request large sequential ranges;
- GUI, CLI, and future Dokany code share the same reader.

These are **enablers**, not benchmark results.

## Known bottlenecks

### Global serialization per backing device

`PhysicalDrive` and `FileBlockDevice` currently protect seek/read operations with a mutex. This is correct for the current synchronous implementation, but it means parallel filesystem requests will serialize at the backing device.

### No metadata caching

Repeated directory traversal can reread inode metadata. A Dokany workload can ask for attributes and directory data many times for the same nodes, so this will matter much more under Explorer than in the current CLI.

### No read window cache

Repeated or overlapping small reads can cause repeated host I/O even when the requested bytes were just read.

### Fixed 64 KiB aligned batch

`read_zone_bytes()` currently limits a lower-level aligned batch to 128 sectors. That number was chosen as a conservative first implementation and has not been tuned.

### Synchronous Windows API

The Windows physical backend uses `SetFilePointerEx` + `ReadFile`. No overlapped I/O or queued request model exists yet.

## Instrument before optimizing

Before changing the I/O strategy, add counters that can answer:

- total logical PFS bytes requested;
- number of `Reader::read()` calls;
- number of APA extent translations;
- number of backing-device reads;
- backing-device bytes read;
- average and maximum backing read size;
- metadata reads versus payload reads;
- cache hit/miss counts once caches exist;
- elapsed wall time for export/mounted reads.

Without these counters it is too easy to "optimize" code that is not on the hot path.

## Benchmark workloads

At minimum, performance comparisons should contain four different workloads.

### A. Large sequential file

Export one large file from a PFS partition to a fast host filesystem.

Measures:

- sequential throughput;
- request coalescing;
- USB/SATA/IDE bridge behavior.

### B. Many small files

Recursively export a tree containing many small files.

Measures:

- metadata overhead;
- inode/directory caching;
- host file creation overhead.

### C. Directory browsing

Enumerate the same large directory repeatedly and resolve file metadata.

Measures:

- metadata cache value;
- frontend overhead independent of bulk copying.

### D. Random mounted reads

Use the future Dokany provider to issue small random reads and attribute queries.

Measures:

- cache behavior;
- concurrency;
- Windows filesystem-provider overhead.

## Comparison rules for pfsshell/pfsfuse

When we publish a comparison, record:

- exact DriveForge commit/version;
- exact pfsshell commit/release;
- physical HDD model/capacity;
- PS2 network adapter or SATA/IDE adapter if relevant;
- USB bridge model and USB link speed if used;
- Windows version;
- source partition and workload;
- host destination medium;
- cold-cache and warm-cache results separately;
- at least three runs when variance matters.

Do not compare a warm DriveForge run to a cold pfsshell run.

## Optimization order

The current preferred order is:

1. counters/trace points;
2. immutable inode metadata cache;
3. directory cache;
4. block/read-window cache;
5. adaptive sequential read size;
6. coalescing reads within one APA extent;
7. Windows offset-based/overlapped device reads;
8. concurrent read scheduling;
9. only then consider deeper platform-specific changes.

A custom Windows kernel filesystem/storage driver is **not** the performance plan. Dokany remains the preferred Explorer bridge; the intended gains are in the userspace data path.

## Correctness gates

An optimization is not accepted if it breaks any of these:

- APA chain validation;
- PFS checksum validation;
- arbitrary unaligned byte reads;
- reads crossing file extents;
- SEGI traversal;
- physical-device read-only invariant;
- sanitizer CI;
- hardware validation cases.

Fast corruption is not a feature.
