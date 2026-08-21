# PS2 DriveForge architecture

## Goal

Build a Windows-first PS2 HDD management stack that can expose an APA/PFS disk to Windows Explorer while keeping parsing, host operations and presentation independent.

The architecture is intentionally different from wrapping pfsshell's interactive device/mount/current-directory model. See [`pfsshell-comparison.md`](pfsshell-comparison.md) for the evidence and the distinction between implemented behavior and future performance targets.

## Current layers

```text
 Native Win32 GUI          CLI          Dokany mount
        |                   |                 |
        +------------- DriveSession ---------+
                            |
                   ps2driveforge_host
                            |
                   ps2driveforge_core
                            |
                +-----------+-----------+
                |           |           |
               APA         PFS       HDL/MBR
                |           |           |
                +-----------+-----------+
                            |
                InstrumentedBlockDevice
                            |
                       BlockDevice
                     /             \
               disk image      PhysicalDriveN
```

`DriveSession` is frontend orchestration, not filesystem state. `InstrumentedBlockDevice` is a transparent diagnostic wrapper, not another on-disk abstraction.

### `BlockDevice`

`BlockDevice` is deliberately byte-addressed. Format code decides sector size and alignment. This avoids leaking Windows handles or PS2SDK/iomanX semantics into filesystem code.

Current source backends are read-only:

- `FileBlockDevice` for images;
- `PhysicalDrive` for `\\.\PhysicalDriveN` on Windows.

The absence of `write()` is a safety boundary. Future mutation should introduce an explicit writable capability rather than quietly widening this interface.

### `InstrumentedBlockDevice`

`InstrumentedBlockDevice` wraps any existing `BlockDevice` and delegates every read while collecting actual backend-request statistics:

- read call count;
- requested bytes;
- failed reads;
- largest read.

Because the wrapper sits below APA/PFS, these numbers represent host/backend reads **after** PFS batching and APA translation. It does not change addressing or source semantics.

### APA

APA owns physical partition-table interpretation:

- 1024-byte header parsing/checksum;
- linked-list traversal;
- partition diagnostics;
- main/sub-partition metadata;
- validation that main and recorded sub-partition extents remain inside the backing device.

It does not know PFS directory/file semantics.

### `ApaVolume`

`ApaVolume` is the translation boundary between PFS logical sub-partition addressing and physical APA extents:

```text
PFS subpart 0 -> APA main extent
PFS subpart 1 -> APA sub extent 0
PFS subpart 2 -> APA sub extent 1
...
```

Only this layer adds a selected extent's physical start LBA. This prevents physical-layout assumptions from leaking upward.

### PFS

The PFS reader owns on-disk filesystem semantics:

```text
superblock
  -> SEGD inode metadata
  -> optional SEGI chain
  -> logical byte stream
  -> directory entries
  -> path resolution
```

It does **not** own Windows filename policy, GUI themes or host file creation.

### `ps2driveforge_host`

The host layer intentionally knows about host/frontend operations rather than raw format parsing. Current responsibilities include:

- recursive PFS export;
- host path construction;
- Windows-invalid character conversion;
- reserved DOS device names;
- case-insensitive collision handling;
- cycle/depth protection;
- cleanup of partial files after failure;
- `DriveSession` orchestration;
- Windows read-only physical-drive discovery;
- Darkness `ReadOnlyMountView` path/list/read mapping.

This separation is important for GUI, Dokany and later drag/drop work: frontends share host policy without contaminating on-disk parsing.

### `DriveSession`

`DriveSession` owns one opened source plus reusable frontend operations around it:

```text
DriveSession
  -> InstrumentedBlockDevice
  -> APA scan result
  -> find partition
  -> PFS browse(partition, path)
  -> PFS stat(partition, path)
  -> PFS read_file(partition, path, offset, span)
  -> host export(partition, path, destination)
  -> operation / backing-I/O statistics
```

The session does **not** maintain a filesystem-global current directory. Paths remain explicit call arguments. GUI navigation state stays in the GUI and concurrent Dokany callbacks resolve independent paths through the same session model.

## Windows GUI boundary

The native GUI is intentionally thin: it opens sources, shows `DriveSession` results, performs navigation/export and renders host UI state.

Darkness adds a Windows-only theme controller beside the GUI rather than inside host/core code:

- persistent **System / Light / Dark** preference;
- System mode follows Windows `AppsUseLightTheme`;
- High Contrast always wins over DriveForge overrides;
- explicit client/TreeView/ListView/header/status palettes;
- documented Windows 11 DWM dark-titlebar support;
- dynamically resolved UxTheme helpers used only as a best-effort enhancement for native dark menus/common controls.

If the optional UxTheme helpers change in a future Windows version, only that visual enhancement may disappear. Filesystem behavior and GUI startup must remain unaffected.

## Darkness Dokany boundary

Dokany callbacks are adapters above `ReadOnlyMountView` and `DriveSession`:

```text
Explorer request
      |
      v
Dokany callback
      |
      v
ReadOnlyMountView
      |
      v
DriveSession
      |
      v
APA/PFS core
```

Callback code converts UTF-16 paths/status values and must not acquire its own knowledge of PFS zones, inode arithmetic or APA sub-partitions.

The mount has multiple independent read-only barriers:

1. no `BlockDevice::write()` API exists;
2. `PhysicalDrive` requests `GENERIC_READ` only;
3. Dokany is mounted with `DOKAN_OPTION_WRITE_PROTECT`;
4. mutation callbacks reject create/write/delete/rename/truncate/metadata writes.

## Release-train implementation map

### 0.1 Ayanami: APA read-only core

Implemented and hardware validated.

### 0.2 Bocchi: PFS read path

Implemented and hardware validated, including direct file/directory read behavior; generated tests cover SEGI and main/sub crossings.

### 0.3 Chisato: Windows browser, host/session layer and hardening

Implemented and hardware validated:

- native Win32 APA/PFS browser;
- read-only physical-drive discovery;
- shared `DriveSession` and recursive host export;
- generated-image end-to-end validation;
- deterministic malformed-metadata corpus;
- backing-read instrumentation;
- real-HDD recursive export and regular-file SHA-256 validation.

### 0.4 Darkness: Dokany read-only Explorer provider

Current development state:

- portable mount namespace/view implemented;
- case-insensitive host aliases implemented;
- thread-safe session stat/random-offset read entry points implemented;
- Dokany 2.3.1 adapter implemented and linked in Windows CI;
- read-only mount executable implemented;
- mutation callbacks and global Dokany write-protect implemented;
- pinned Dokany SDK/runtime CI install and packaging implemented;
- System/Light/Dark native GUI theming implemented;
- portable mount-view tests and both CI platforms green;
- real-HDD Explorer mount validation still pending.

See [`darkness-plan.md`](darkness-plan.md).

## Dependency rules

These are architecture invariants, not suggestions:

1. `ps2driveforge_core` must not include Win32 GUI/Dokany/host-path policy.
2. PFS must access physical data through `ApaVolume`, never by adding APA LBAs itself.
3. Frontends must not reimplement APA/PFS parsing.
4. Windows filename conversion must not alter PFS-visible names.
5. Read-only parsing must remain usable without loading GUI or Dokany code.
6. `DriveSession` may orchestrate parser/host operations but must not become a shell-global mount/current-directory model.
7. Instrumentation/caches must remain transparent to parser correctness and safety checks.
8. GUI theme/platform policy must stay above the host/core filesystem layers.
9. Future write support must be a separate capability with backup/recovery semantics.

If a feature appears to require violating one of these, update the architecture deliberately rather than creating an accidental dependency.

## Important address units

When debugging PFS, write down the unit at each step. The code handles several incompatible address spaces:

```text
host bytes
  <-> 512-byte PS2 sectors
  <-> 1024-byte PFS metadata blocks
  <-> PFS zones
  <-> APA logical extents
  <-> physical disk LBA
```

Common mistakes:

- interpreting `BlockInfo.number` as a sector for file data;
- using payload zone arithmetic for inode metadata;
- pre-adding an APA extent start before calling `ApaVolume`;
- assuming APA sub-partitions are physically contiguous.

See [`apa-format-notes.md`](apa-format-notes.md) and [`pfs-format-notes.md`](pfs-format-notes.md).

## Current concurrency/performance reality

Dokany may issue concurrent callbacks, so no frontend-global PFS current directory is introduced. Backing I/O is still deliberately conservative:

```text
DriveSession
  -> InstrumentedBlockDevice
    -> PhysicalDrive::read
      -> per-device mutex
      -> SetFilePointerEx
      -> synchronous ReadFile
```

The image backend similarly serializes one `ifstream` seek/read state.

That is acceptable for Darkness correctness work and is a known performance limit. Cache design, request coalescing, read-ahead and overlapped physical I/O belong to 0.5 Emilia and should be driven by the backing-I/O statistics collected during Explorer workloads.

## Explorer namespace

Darkness currently exposes:

```text
P:\
  Partitions\
    __system\
    __common\
    +OPL\
    ...
```

`Games\` and HDL virtual ISO views remain 0.7 Guts. Synthetic recovery/MBR views remain later work.

Explicit namespaces avoid collisions between real partition names and future synthetic views while allowing the GUI to remain friendlier and flatter.

## Safety invariants before write support

Before a physical-disk write path exists, the project requires:

1. parser fuzz/unit tests;
2. metadata backup format;
3. read-only-by-default physical device access;
4. explicit validation of every target extent;
5. journaling/transaction or other interruption-recovery strategy for APA/PFS metadata writes;
6. destructive testing against disposable images before real HDDs;
7. explicit user opt-in to writable mode.

The current source-device path remains read-only end to end.

## Debugging starting points

| Symptom | First layer to inspect |
| --- | --- |
| APA not detected / chain stops | `apa.cpp` + `apa-format-notes.md` |
| valid header but impossible extent | APA extent-bounds diagnostics |
| PFS superblock invalid | `pfs::probe()` |
| inode checksum/magic failure | PFS metadata addressing |
| wrong data but valid inode | zone -> sector arithmetic / SEGD-SEGI traversal |
| only one sub-partition fails | `ApaVolume` logical extent translation |
| directory garbage | 512-byte dentry boundary parsing |
| export path/name failure | `ps2driveforge_host`, not PFS core |
| CLI/GUI disagreement | `DriveSession` call inputs/results first |
| direct reader works but Explorer lookup fails | `ReadOnlyMountView` / Dokany adapter |
| Explorer copy differs from direct export | Dokany random-offset reads, then PFS byte-range read |
| slow mounted reads | backing-I/O stats + `performance.md` |
| GUI-only colour/rendering issue | Windows theme/UI boundary, never APA/PFS |

The goal is to identify which abstraction is wrong before adding compatibility hacks to the layer above it.
