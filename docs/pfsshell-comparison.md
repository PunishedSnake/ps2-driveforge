# DriveForge and pfsshell: architecture comparison

This document explains why PS2 DriveForge exists when `pfsshell` already provides mature APA/PFS access.

The goal is not to diminish `pfsshell`. It remains an important compatibility and format reference, and its tree also contains `pfsfuse`. DriveForge solves a different host-side problem: a Windows-first, library-first PS2 HDD stack for native GUI workflows, Explorer integration, diagnostics, high-throughput HDL/PFS mutation and FHDB Manager-compatible recovery without exposing PS2SDK/iomanX process state to every frontend.

## Ground rules

Claims are split into:

- **Implemented / observed** - behavior present in DriveForge or directly visible in referenced pfsshell source.
- **Design target** - an architectural opportunity that must not be described as a measured speedup until benchmarks exist.

"Faster than pfsshell" without workload, device, connection, build/version, cache state and measurements is not an acceptable project claim. Computers already generate enough folklore without benchmark fan fiction.

## pfsshell model

The traditional pfsshell workflow is naturally shell-oriented:

```text
select device
 -> mount partition
 -> maintain current PFS directory
 -> ls/get/put/etc.
 -> unmount
```

The shell maintains device, mount and current-path state and routes operations through its host iomanX adapter plus ported APA/PFS/HDL layers. This is a sensible compatibility design because it reuses PS2-side semantics.

`pfsfuse` provides a filesystem frontend separately. The pfsshell documentation also describes Windows use through a Dokan FUSE wrapper, mounting a selected PFS partition to a drive letter.

References:

- <https://github.com/ps2homebrew/pfsshell>
- <https://github.com/ps2homebrew/pfsshell/blob/master/README.md>
- <https://github.com/ps2homebrew/pfsshell/blob/master/src/shell.c>
- <https://github.com/ps2homebrew/pfsshell/blob/master/src/iomanx_adapter.c>

## DriveForge model

DriveForge keeps orchestration explicit rather than reproducing shell-global mount/current-directory state:

```text
WinUI / Win32 / CLI / Dokany
             |
      ps2driveforge_host
      /       |        \
DriveSession  |   management/deploy/recovery coordinators
              |
       ps2driveforge_core
 APA / PFS / HDL / writers / recovery formats
              |
       BlockDevice capability boundary
        /                    \
read-only PhysicalDrive   explicit WritableBlockDevice
                          /                       \
writable image      guarded WritablePhysicalDrive
```

A `DriveSession` accepts explicit partition/path/range operations. Frontend navigation stays in the frontend. APA physical placement stays in `ApaVolume` / `WritableApaVolume`, and recovery artifacts remain format modules rather than UI inventions.

## Implemented differences useful for our target

These are architecture/functionality statements, not universal throughput claims.

### 1. Read and write capabilities are structurally separate

`BlockDevice` remains read-only. The ordinary Windows `PhysicalDrive` remains `GENERIC_READ`.

Mutation requires `WritableBlockDevice`. Disk images use `WritableFileBlockDevice`; Frieren also has a separately admitted `WritablePhysicalDrive` that requires a read-only fingerprint/APA/GPT preflight, a fresh RW identity check, Windows volume locking/dismount and another identity check before it becomes usable.

That means adding physical write support did not quietly turn every parser, mount and browser into a writer. A surprisingly low bar, yet storage software has found ways to trip over it historically.

### 2. APA extents are normalized and validated centrally

PFS logical `(subpart, address)` access is translated through `ApaVolume` and `WritableApaVolume`. APA rejects recorded main/sub extents outside the backing device before PFS receives them.

HDL main plus authoritative subs are also one logical management group, which allows the UI to display and delete one game without exposing allocation bookkeeping as a scavenger hunt.

### 3. Native HDL install/remove does not shell out

Frieren plans APA allocation in-process, writes/verifies the ISO payload into unpublished extents, builds DEADFEED metadata and publishes APA visibility last. Multipart main/sub layouts have deterministic end-to-end coverage.

Removal unlinks the selected main and its authoritative subs by rewriting only surviving APA neighbour headers. Game payload does not need a ceremonial zero-fill, so delete cost is metadata-sized rather than ISO-sized.

### 4. Native PFS writes are library operations

The writer supports cached bitmap allocation, create/replace, copy-on-write replacement, directory creation/growth, fragmented direct extents, SEGI, tree removal, batch writes and cold-reader verification.

OPL loose files and TAR containers are therefore deployed by the same native storage stack instead of starting a new shell process for every small asset.

### 5. Byte-range reads remain first-class

Mounted Windows reads can begin and end at arbitrary byte offsets. DriveForge's PFS reader presents one logical byte stream across SEGD/SEGI descriptors and APA extents rather than requiring the frontend to emulate PS2-sector-aligned shell operations.

### 6. Host policy is not parser policy

Windows reserved names, filename conversion, case-insensitive collisions, recursive host export, SetupAPI discovery, UAC, theme behavior, Dokany and user confirmation stay above `ps2driveforge_core`.

PS2-visible names and on-disk interpretation remain format concerns.

### 7. Recovery is interoperable with FHDB Manager

Frieren distinguishes the shared FHDB Rescue Capsule (`PS2HBRC\0`) from DriveForge's private Mutation Journal (`PS2DFRC1`). It also implements the shared `HDDMBR`, `HDDRAW`, `HDDMETA/APAMETA1` and `FORENSIC.TXT` contracts plus image-level guarded restore/APA repair.

Physical mutation coordinators preserve an FHDB-compatible master backup before write and use the DriveForge journal for the exact metadata transaction where applicable. Those artifacts solve different problems and therefore remain different formats, which is refreshingly less confusing than naming both of them "recovery.dat".

### 8. Cross-layer behavior is reproducible

The suite contains generated APA/PFS images, corruption cases, fault-oriented writer tests, full HDL/PFS/TAR deployment tests, recovery format tests and sanitizer CI. Real-HDD testing remains a separate release gate because no unit test can convincingly emulate a USB bridge deciding to reinterpret reality.

## Performance status

Emilia already replaced the earlier shared seek-pointer model with explicit-offset Windows I/O, validated metadata caches, read-ahead and instrumentation. The preserved real-HDD baseline is in [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md).

Frieren additionally removes several architectural sources of write overhead:

```text
no pfsshell subprocess per operation
no FUSE boundary for native management writes
one retained PFS writer session for batches
lazy cached bitmap chunks
large sequential payload batches
metadata-only APA deletion
no mandatory complete HDL-list rebuild after known deletion
```

These properties explain what should be measured. They are not a license to invent an N-times-faster number before comparative testing on the same host and disk.

## Explorer namespace

The read-only Explorer view remains a separate presentation capability:

```text
PS2HDD\
  Partitions\
    __system\
    __common\
    +OPL\
```

HDL Tools and recovery operations belong in the application UI rather than being smuggled into arbitrary Explorer write callbacks. Windows Explorer is useful, but it does not need sector-zero privileges merely because someone pressed Delete.

## Development rule

Whenever a change is called "faster" or "better than pfsshell":

1. identify the specific old UX or bottleneck;
2. identify the DriveForge layer that changes it;
3. retain correctness and recovery coverage;
4. measure counters and timings under the same conditions;
5. record the result rather than the hoped-for result.

The useful distinction is not old tool versus new tool. It is compatibility reference versus an architecture deliberately designed for a different host workflow.
