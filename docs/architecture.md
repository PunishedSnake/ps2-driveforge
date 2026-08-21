# PS2 DriveForge architecture

## Goal

Build a Windows-first PS2 HDD management stack that can expose an APA disk to Windows Explorer while keeping parsing, host operations and presentation independent.

The architecture is intentionally different from wrapping pfsshell's interactive device/mount/current-directory model. See [`pfsshell-comparison.md`](pfsshell-comparison.md) for the evidence and the distinction between implemented behavior and future performance targets.

## Current layers

```text
 Native Win32 GUI          inspector CLI          mount CLI
        |                       |                     |
        |                       |            DokanyMountController
        |                       |                     |
        +--------------- DriveSession / host --------+
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

Windows disk-interface discovery and UAC are frontend/platform concerns. They select or permit access to a `BlockDevice`; they never alter APA/PFS format semantics.

`DriveSession` is frontend orchestration, not filesystem state. `InstrumentedBlockDevice` is a transparent diagnostic wrapper, not another on-disk abstraction.

### `BlockDevice`

`BlockDevice` is deliberately byte-addressed. Format code decides sector size and alignment. This avoids leaking Windows handles or PS2SDK/iomanX semantics into filesystem code.

Current source backends are read-only:

- `FileBlockDevice` for images;
- `PhysicalDrive` for `\\.\PhysicalDriveN` on Windows.

The absence of `write()` is a safety boundary. Future mutation should introduce an explicit writable capability rather than quietly widening this interface.

`PhysicalDrive` records the Win32 error from a failed raw open so the Windows frontend can distinguish access denial from other failures. This diagnostic addition does not change the handle flags: raw disks continue to use `GENERIC_READ` only.

### Windows physical-device discovery

Darkness removes the old development-only `PhysicalDrive0..31` guess loop from the GUI.

The platform discovery path is:

```text
SetupDiGetClassDevs(GUID_DEVINTERFACE_DISK)
  -> SetupDiEnumDeviceInterfaces
  -> disk interface path
  -> IOCTL_STORAGE_GET_DEVICE_NUMBER
  -> actual PhysicalDriveN
  -> PhysicalDrive(GENERIC_READ)
  -> APA Reader
```

The temporary device-interface handle requests no disk-data access; it exists only to ask Windows which storage-device number backs that concrete interface. The resulting raw disk is reopened through the normal DriveForge backend and classified by the APA parser.

Consequences:

- no arbitrary maximum disk number in the GUI discovery path;
- no attempts to open nonexistent guessed numbers;
- Windows model/friendly name can be shown without using it as format evidence;
- only parser-confirmed PS2 APA candidates are presented as PS2 HDD choices.

### GUI elevation boundary

Raw physical-disk access may require Administrator rights. DriveForge deliberately avoids a global `requireAdministrator` manifest because disk-image workflows do not inherently require elevation.

The native GUI uses a controlled relaunch:

```text
normal process
  -> ShellExecuteExW("runas", --elevated-relaunch)
  -> elevated GUI
  -> SetupAPI discovery
```

The internal relaunch marker prevents an elevation loop. If UAC is cancelled, the unelevated process continues in a limited image-capable mode and exposes a manual `Restart as Administrator` command.

Elevation belongs to the Windows frontend. Core/parser/session code does not inspect process tokens.

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

It does **not** own Windows filename policy or host file creation.

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
- portable read-only mounted namespace mapping through `ReadOnlyMountView`.

This separation is important for GUI and Dokany work: frontends share host policy without contaminating on-disk parsing.

### `DriveSession`

`DriveSession` owns one opened source plus the reusable frontend operations around it:

```text
DriveSession
  -> InstrumentedBlockDevice
  -> APA scan result
  -> find partition
  -> PFS browse(partition, path)
  -> random-offset PFS read(partition, path, offset, span)
  -> host export(partition, path, destination)
  -> operation / backing-I/O statistics
```

The session does **not** maintain a filesystem-global current directory. Paths remain explicit call arguments. GUI navigation state stays in the GUI; Dokany callbacks resolve independent paths through the same session model.

### Darkness Dokany boundary

Darkness keeps Dokany above `ReadOnlyMountView`/`DriveSession`. The adapter translates Windows filesystem callbacks into portable lookup/list/read operations and maps their result back to NTSTATUS.

The callback implementation and mount lifecycle now live in a shared `DokanyMountController`:

```text
PS2-DriveForge.exe          PS2-DriveForge-Mount.exe
         |                            |
         +---- DokanyMountController -+
                         |
                  ReadOnlyMountView
                         |
                    DriveSession
```

This is deliberate. The GUI does **not** spawn the mount executable and the CLI no longer owns a second callback implementation. The standalone executable remains useful for headless scripting and `--debug` traces, while both frontends exercise identical filesystem-provider code.

The controller owns:

- the second read-only source handle used by the mounted filesystem;
- `DriveSession` / `ReadOnlyMountView` lifetime;
- the `DokanMain` worker thread;
- mounted/running/error state;
- mountpoint normalization and automatic free-drive-letter selection;
- clean unmount requests.

One important boundary is `ZwCreateFile`: Dokany passes NT kernel `FILE_*` create-disposition values, **not** Win32 `CreateFileW` constants. These two APIs have overlapping numeric values with different meanings (`FILE_OPEN == 1`, while Win32 `CREATE_NEW == 1`). The first real Darkness mount exposed that trap when the root open was returned as `STATUS_OBJECT_NAME_COLLISION` and Explorer reported "The file exists."

The NT create-disposition contract is represented explicitly in `src/mount/dokany_open_policy.hpp` and regression-tested on both Windows and Linux. This policy module decides only whether a read-only open is semantically permitted; it does not parse PFS or touch storage.

The mount maintains four independent read-only barriers:

```text
no BlockDevice::write()
        +
PhysicalDrive GENERIC_READ
        +
DOKAN_OPTION_WRITE_PROTECT
        +
mutation/create/overwrite callback rejection
```

### Win32 theme boundary

Darkness' System/Light/Dark controller remains in the native GUI layer. Windows DWM/UxTheme behavior does not enter `DriveSession`, the host mount namespace, or APA/PFS code. A future non-Windows frontend can map its own System theme to the desktop environment without changing storage logic.

## Release-train implementation map

### 0.1 Ayanami: APA read-only core

Implemented and hardware validated.

### 0.2 Bocchi: PFS read path

Implemented and hardware validated, with real-hardware SEGI remaining a separately documented coverage gap.

### 0.3 Chisato: Windows browser and shared host/session layer

Implemented and hardware validated on the real 149.05 GiB test HDD, including discovery, GUI browse, recursive `+OPL` export, a regular PFS file export and backing-I/O baseline.

### 0.4 Darkness: Explorer mount and Windows device workflow

Implemented/validated so far:

- portable `ReadOnlyMountView`;
- random-offset/thread-safe session operations for filesystem callbacks;
- Dokany 2.3.1 provider;
- shared `DokanyMountController` used by GUI and mount CLI;
- structural read-only mount barriers;
- Windows-safe visible aliases;
- native System/Light/Dark GUI theme;
- SetupAPI disk-interface enumeration and APA candidate filtering;
- controlled GUI elevation/relaunch;
- direct GUI mount/open-in-Explorer/unmount actions;
- portable regression coverage for NT `ZwCreateFile` open disposition semantics;
- real CLI-driven Explorer browse/copy/write-rejection/clean-unmount validation.

The remaining 0.4 validation is the integrated GUI path on the real machine.

## Dependency rules

These are architecture invariants, not suggestions:

1. `ps2driveforge_core` must not include Win32 GUI/Dokany/SetupAPI/UAC/host-path policy.
2. PFS must access physical data through `ApaVolume`, never by adding APA LBAs itself.
3. Frontends must not reimplement APA/PFS parsing.
4. Windows filename conversion must not alter PFS-visible names.
5. Read-only parsing must remain usable without loading GUI/Dokany code.
6. `DriveSession` may orchestrate parser/host operations but must not become a shell-global mount/current-directory model.
7. Instrumentation/caches must remain transparent to parser correctness and safety checks.
8. Dokany callback policy may translate NT semantics but must not become another filesystem parser.
9. GUI and mount CLI must share the same Dokany callback/controller implementation.
10. Windows device discovery may select a physical backend but must not infer PS2 format identity without the APA parser.
11. Future write support must be a separate capability with backup/recovery semantics.

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

The API is designed so Dokany callbacks do not require a global shell mount/current directory, but backing I/O is not yet highly concurrent.

Today:

```text
DriveSession
  -> InstrumentedBlockDevice
    -> PhysicalDrive::read
      -> per-device mutex
      -> SetFilePointerEx
      -> synchronous ReadFile
```

The image backend similarly serializes one `ifstream` seek/read state.

This is correct for Darkness and is a known performance limit. Explorer hardware validation has already shown a metadata-heavy callback pattern, making caching/coalescing an evidence-based Emilia target. See [`performance.md`](performance.md) before changing batching, caching or Windows I/O primitives.

## Explorer namespace

Current Darkness namespace:

```text
P:\
  Partitions\
    __system\
    __common\
    +OPL\
  Games\                # synthetic HDL view, later
  System\
    MBR.bin              # controlled metadata/recovery view, later
```

Only `Partitions` is active in Darkness. `Games` and `System` are reserved design targets for later releases.

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
| PS2 disk missing from GUI | SetupAPI discovery / raw-open error / elevation |
| wrong disk shown as PS2 | APA detection, never device friendly-name policy |
| APA not detected / chain stops | `apa.cpp` + `apa-format-notes.md` |
| valid header but impossible extent | APA extent-bounds diagnostics |
| PFS superblock invalid | `pfs::probe()` |
| inode checksum/magic failure | PFS metadata addressing |
| wrong data but valid inode | zone -> sector arithmetic / SEGD-SEGI traversal |
| only one sub-partition fails | `ApaVolume` logical extent translation |
| directory garbage | 512-byte dentry boundary parsing |
| export path/name failure | `ps2driveforge_host`, not PFS core |
| CLI/GUI disagreement | `DriveSession` call inputs/results first |
| mount point exists but Explorer says "The file exists" | Dokany `ZwCreateFile` NT disposition mapping |
| mounted path fails but direct browse/export works | `ReadOnlyMountView` / `DokanyMountController` |
| GUI mount and CLI mount disagree | shared controller inputs/lifetime; callbacks should be identical |
| slow sequential reads | instrumentation + `performance.md` |
| GUI-only issue | Win32 presentation/elevation/discovery; reproduce storage operation below GUI first |

The goal is to identify which abstraction is wrong before adding compatibility hacks to the layer above it.
