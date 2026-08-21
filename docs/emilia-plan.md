# Emilia 0.5 development plan

Emilia is the performance release above the hardware-validated Darkness stack. It keeps every source read-only invariant intact while aggressively reducing redundant metadata I/O, improving sequential/random read behavior, and modernizing the Windows frontend without moving storage logic into the UI layer.

## Definition of done

The complete Darkness read-only workflow must remain bit-correct while measurable Explorer and extraction workloads perform materially less backing I/O and/or complete faster. Every accepted optimization needs before/after counters and must keep generated-image SHA-256, corruption regressions, sanitizer CI, Dokany write protection, and real-HDD read-only behavior green.

The Windows UI modernization is a parallel frontend goal: a WinUI 3 frontend must reach functional parity with the existing Win32 GUI before it becomes the default. The old Win32 frontend remains a fallback during the migration.

Emilia also establishes explicit usability/performance comparison workloads against pfsshell-oriented workflows and HDL Batch Installer's HDD Manager. The goal is not a vague claim that DriveForge is "faster": common management operations must require fewer blocking steps, fewer redundant device reads, and no per-partition helper-process startup.

## Performance workstreams

1. **Instrumentation first**
   - backend service time and average read latency;
   - current/max in-flight backing reads;
   - high-level scan/browse/stat/read/export elapsed time;
   - cache hit/miss/eviction counters;
   - later metadata/payload classification and read-ahead usefulness counters.

2. **Immutable PFS/session caches**
   - validated PFS probe cache per partition;
   - resolved-node/inode cache;
   - directory enumeration cache;
   - stat/path cache where it removes repeated Explorer metadata walks;
   - explicit cache clear for cold/warm benchmark separation.

3. **Read-window/block cache**
   - bounded memory use;
   - overlapping small reads served from RAM;
   - counters for hit/miss/eviction/wasted prefetched bytes;
   - tuning based on real workloads instead of fixed folklore values.

4. **Read-ahead and request coalescing**
   - detect sequential access;
   - coalesce adjacent physical reads inside one APA extent;
   - never merge across unsafe logical/physical boundaries;
   - tune windows separately for image files and physical HDDs where useful.

5. **Windows physical I/O rewrite**
   - replace shared `SetFilePointerEx` state with offset-based I/O;
   - use `FILE_FLAG_OVERLAPPED` and per-request offsets/events;
   - remove the global seek/read mutex where correctness no longer requires it;
   - measure queue depth 1/2/4/8 rather than assuming more concurrency is better for old HDDs/bridges.

6. **Instant partition catalog / HDD Manager model**
   - build the complete management list directly from the already validated APA `ScanResult`;
   - creating/filtering/sorting the list performs zero additional device I/O;
   - PFS details and HDL game metadata are optional enrichment, never prerequisites for first paint;
   - enrichment runs lazily/in batches and may update visible rows asynchronously;
   - sub-partitions can be shown/hidden without rescanning the disk;
   - the eventual writable HDD Manager will use this same immutable catalog as its selection model before any mutation capability is considered.

7. **Benchmark harness**
   - preserve Darkness cold GUI/Explorer workload;
   - repeated directory/stat workload;
   - large sequential file workload;
   - many-small-files workload;
   - random mounted reads;
   - cold and warm measurements kept separate;
   - time-to-first-complete-partition-list after APA scan;
   - time-to-optional-HDL-title-enrichment kept separate from list availability.

## Competitor workflow target

HDL Batch Installer's current HDD Manager calls `PFSSHELL.lspart()` and then, when game titles are enabled, loops over main HDL partitions and launches a synchronous `HDL.EXE info hddN: <partition>` process for each one. That means the dialog's fully enriched initialization cost grows with the number of game partitions and blocks on repeated helper-process startup/device work.

DriveForge's contract is deliberately different:

```text
one APA scan
  -> immutable PartitionCatalog
  -> complete management rows immediately
  -> UI first paint
  -> optional lazy/batched enrichment
```

Opening a deletion/management list must never require probing every PFS filesystem, mounting partitions, or launching one helper process per HDL game. Sorting/filtering/selecting rows must remain memory-only operations.

pfsshell remains a compatibility/reference implementation, but DriveForge frontends must not inherit its selected-device/current-mount/current-directory shell state or require shell-command round trips for ordinary GUI navigation.

## Preserved hardware baseline

Darkness handed Emilia this validated workload:

```text
cold GUI start
 -> UAC/discovery/auto-open
 -> mount
 -> root
 -> Partitions
 -> +OPL
 -> __common\OPL
 -> read/copy conf_hdd.cfg
 -> unmount
```

Historical Chisato `+OPL` browse baseline:

```text
Backing read calls:   215
Backing bytes read:   206.50 KiB
Average backing read: 983 B
Largest backing read: 1.00 KiB
Failed backing reads: 0
```

These numbers are a reference, not a claim that different frontend workloads are directly comparable.

## WinUI 3 modernization

As of August 2026 the target is WinUI 3 from Windows App SDK 2.3.1, not UWP. WinUI 3 is a normal desktop UI framework and can be deployed unpackaged. Core/host/mount libraries remain ordinary C++20 code and do not gain XAML/Windows App SDK dependencies.

Migration strategy:

```text
ps2driveforge_core
      |
ps2driveforge_host
      |
shared Windows services
(discovery/mount/catalog/performance model)
      |
      +---- legacy Win32 frontend (temporary fallback)
      |
      +---- WinUI 3 C++/WinRT frontend (normal user)
                    |
                    +---- elevated read-only raw-disk broker
```

The WinUI project is expected to be an MSBuild/C++/WinRT frontend consuming the same native libraries. Windows App SDK's C++ build tooling is still MSBuild/NuGet-oriented; do not contort the cross-platform CMake storage build around XAML generation.

The current whole-GUI `runas` model is not the long-term WinUI architecture. Windows App SDK dynamic-dependency/bootstrap behavior and least-privilege goals make a small elevated raw-disk broker preferable. The WinUI shell remains non-elevated; only the broker opens `PhysicalDriveN`, still `GENERIC_READ` only, and exposes a narrowly scoped read-only IPC surface. Disk-image workflows never need elevation.

Initial WinUI parity target:

- startup disk discovery without blocking the UI thread;
- detected PS2 HDD cards/list and image opening;
- instant APA partition catalog;
- lazy HDL/PFS metadata enrichment with visible progress but no modal wait;
- APA partition navigation;
- PFS directory/file browsing;
- export;
- read-only mount/open-in-Explorer/unmount;
- System/Light/Dark behavior through native WinUI theme resources;
- status/performance dashboard;
- clear read-only state;
- fast filtering/search/sort over the in-memory partition catalog.

Only after parity and hardware validation should the WinUI executable replace the legacy Win32 executable in the normal package.

## Deployment direction for WinUI

Prefer an unpackaged or self-contained WinUI 3 desktop frontend so DriveForge can retain a traditional portable/installer workflow. Windows App SDK runtime handling must be explicit in packaging: framework-dependent runtime installer or self-contained deployment. Do not silently assume the runtime exists, and do not rely on elevating the WinUI shell itself as the raw-disk access strategy.

## Correctness/safety gates

No performance or UI change is accepted if it breaks:

- APA checksum/link/bounds validation;
- PFS checksum/superblock validation;
- SEGD/SEGI traversal;
- arbitrary unaligned file reads;
- APA main/sub translation;
- generated-image export SHA-256;
- corruption corpus behavior;
- source `GENERIC_READ` invariant;
- absence of public source `write()` API;
- Dokany write-protect/mutation rejection;
- clean mount/unmount lifecycle;
- Windows/MSVC CI;
- Linux Clang ASan+UBSan+`-Werror` CI.

Fast corruption is still not a feature.
