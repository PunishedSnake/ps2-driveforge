# Emilia 0.5 development plan

Emilia is the performance release above the hardware-validated Darkness stack. It keeps every source read-only invariant intact while aggressively reducing redundant metadata I/O, improving sequential/random read behavior, and modernizing the Windows frontend without moving storage logic into the UI layer.

## Definition of done

The complete Darkness read-only workflow must remain bit-correct while measurable Explorer and extraction workloads perform materially less backing I/O and/or complete faster. Every accepted optimization needs before/after counters and must keep generated-image SHA-256, corruption regressions, sanitizer CI, Dokany write protection, and real-HDD read-only behavior green.

The Windows UI modernization is a parallel frontend goal: a WinUI 3 frontend must reach functional parity with the existing Win32 GUI before it becomes the default. The old Win32 frontend remains a fallback during the migration.

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
   - move to `FILE_FLAG_OVERLAPPED`/OVERLAPPED reads after baseline instrumentation is in place;
   - remove the global seek/read mutex where correctness no longer requires it;
   - measure queue depth 1/2/4/8 rather than assuming more concurrency is better for old HDDs/bridges.

6. **Benchmark harness**
   - preserve Darkness cold GUI/Explorer workload;
   - repeated directory/stat workload;
   - large sequential file workload;
   - many-small-files workload;
   - random mounted reads;
   - cold and warm measurements kept separate.

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
(discovery/elevation/mount controller)
      |
      +---- legacy Win32 frontend
      |
      +---- WinUI 3 C++/WinRT frontend
```

The WinUI project is expected to be an MSBuild/C++/WinRT frontend consuming the same native libraries. Windows App SDK's C++ build tooling is still MSBuild/NuGet-oriented; do not contort the cross-platform CMake storage build around XAML generation.

Initial WinUI parity target:

- startup UAC/discovery behavior;
- detected PS2 HDD list and image opening;
- APA partition navigation;
- PFS directory/file browsing;
- export;
- read-only mount/open-in-Explorer/unmount;
- System/Light/Dark behavior through native WinUI theme resources;
- status/performance counters;
- clear read-only state.

Only after parity and hardware validation should the WinUI executable replace the legacy Win32 executable in the normal package.

## Deployment direction for WinUI

The preferred development path is an unpackaged WinUI 3 desktop frontend so DriveForge can retain a traditional portable/installer workflow and raw-disk elevation model. Windows App SDK runtime handling must be explicit in packaging: framework-dependent runtime installer or self-contained deployment. Do not silently assume the runtime exists.

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
