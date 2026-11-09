# Frieren native PFS write path

Frieren 0.6 introduces the first native DriveForge PFS mutation layer. The goal is not to wrap `pfsshell`, mount the filesystem through FUSE or replay one tiny command at a time. DriveForge already owns validated APA/PFS structures, so the image writer stays in-process and works from the same on-disk rules as the reader.

This document describes the current image-only milestone. It is not permission to enable physical HDD writes. A working image writer and a safe raw-disk writer are related problems, not the same problem wearing a more alarming handle.

## Safety model

PFS mutation is built on three boundaries:

```text
WritableBlockDevice
 -> WritableApaVolume
 -> PFS ImageWriter / planners
```

`WritableApaVolume` owns main/sub extent translation and validates exact sector counts, logical subpart bounds, physical disk bounds and overflow before forwarding writes.

PFS writers reuse the normal reader for preflight and post-write verification. They do not maintain a permissive private parser for output they just created. If the read path rejects a result, the writer does not get to declare victory through professional courtesy.

Normal PFS mutation also remains separate from exceptional recovery. `WriteTransaction` may protect small metadata ranges, but FHDB Rescue Capsule and APA forensic artifacts belong to the recovery domain documented in [`fhdb-bootstrap-parity.md`](fhdb-bootstrap-parity.md).

## Performance model

The writer is shaped around costs that make older host workflows unpleasant on large PS2 disks:

- one validated PFS probe is reused across an `ImageWriter` session;
- bitmap metadata is loaded lazily in 1024-byte chunks and cached by `(subpart, chunk)`;
- related small allocations reuse cached bitmap state instead of rescanning the filesystem for each file;
- payload data is transferred in large sequential batches where layout permits;
- metadata changes use narrow before-image transactions with flush and byte readback;
- existing regular files are replaced copy-on-write;
- multi-file imports reuse one writer through `pfs::write_batch()`;
- final paths are resolved and checked through the normal PFS reader.

These are architectural properties, not invented benchmark multipliers. Comparative release claims wait for same-host, same-disk measurements.

## File creation

The create path is broadly:

1. resolve and validate the parent directory through the normal reader;
2. plan dentry placement and required allocation before publishing a name;
3. reserve inode/data zones through the bitmap layer;
4. write and flush payload data;
5. read payload bytes back;
6. publish checksummed inode metadata;
7. publish the directory entry only after underlying content is valid;
8. cold-resolve the resulting path and verify bytes through the normal reader.

A failure before dentry publication cannot expose a half-written file by name. Cleanup failure prefers leaked allocated space over a live directory entry referencing unverified data. Wasted space is annoying; live corruption is a more ambitious problem.

## File replacement

Existing regular files use copy-on-write:

1. validate the old inode/extents;
2. allocate replacement storage without changing the live inode;
3. write, flush and read back replacement data;
4. atomically switch inode metadata to the new layout;
5. verify the path and bytes through a fresh reader;
6. release old zones only after the new file is live and verified.

If old-zone release fails, the replacement remains valid and the operation reports leaked space. Old live storage is never freed before the inode stops referencing it.

## Directory creation and growth

`ImageWriter::ensure_directory()` creates missing components recursively while reusing the validated probe and bitmap cache.

Directory creation builds a normal directory inode and initializes `.` / `..` entries before parent publication.

Frieren also contains direct directory growth support for existing directories that exhaust reusable dentry space. Growth is planned through the same extent/bitmap machinery and keeps existing directory data readable while new capacity is introduced. A directory does not become a special case where bounds and verification are suspended because filenames needed more room.

## Fragmented extents and SEGI

The early writer only accepted one convenient contiguous run. Current Frieren has advanced extent-layout and SEGI write support for cases that cannot be represented by the small direct descriptor set alone.

The writer distinguishes metadata block units from file-data zone units and preserves APA subpart mapping through `WritableApaVolume`. Tests specifically cover SEGI and fragmented layouts because this is precisely where a casual `number * sector_size` tends to become archaeology.

Unsupported or structurally ambiguous layouts still fail closed. Advanced support means more valid layouts are representable, not that every damaged inode receives an inspirational interpretation.

## Tree removal

PFS tree removal is distinct from APA partition removal.

PFS removal updates filesystem directory/inode/bitmap state inside one PFS volume. APA removal unlinks a whole main partition and its owned sub headers from the disk-level APA chain. Mixing the two because both operations contain the verb "delete" would be a rather expensive abstraction leak.

Tree-removal tests protect recursive ownership, allocation release and reader-visible results.

## Batch writes and OPL asset import

`pfs::write_batch()` executes many operations through one writer session while preserving per-file safe publication and verification.

The host-side `opl_pfs_import` layer remains separate from HTTP fetching and low-level PFS format code:

```text
provider fetch/staging
 -> immutable destination preflight
 -> normalize and deduplicate paths
 -> freeze host bytes
 -> resolve/create directories
 -> cached PFS batch mutation
 -> normal-reader verification
```

Cache-only provider data is not written to PFS. HDD-OSD metadata remains a different placement domain.

## TAR destinations

Frieren now includes deterministic TAR archive handling for OPL variants that use `ART/art.tar`, `CFG/cfg.tar` or `CHT/cht.tar`.

Provider staging still deals in individual logical assets. The destination layer owns TAR parse/update/serialization, then writes the resulting archive as a normal PFS file through the PFS writer. This preserves the boundary between "downloaded member" and "on-disk container" rather than pretending `art.tar :: GAME_COV.png` is a magical filesystem path.

TAR behavior has dedicated regression coverage and remains a destination policy, not a network-provider feature.

## PC partition-map safety gate

`disk_layout_guard` inspects conventional PC partitioning sectors and distinguishes ordinary legacy MBR evidence from protective GPT, hybrid GPT/MBR and orphan GPT evidence.

Conflicting GPT ownership is a hard image-mutation refusal. This guard does not replace APA validation. A future physical writer must pass both the PC-layout ownership gate and clean positive PS2 APA identification.

## APA partition removal

Deleting an APA main partition is deliberately independent from PFS file deletion and HDL metadata enrichment.

`plan_remove_main_partition()` validates a clean chain, collects the selected main plus authoritative owned sub headers and computes only required surviving link rewrites. `remove_main_partition_from_image()` applies those small changes and verifies the resulting APA chain.

Detached headers/payload bytes are left physically untouched and become unreachable free space available to future allocation. Zero-filling a multi-gigabyte game so the linked list can forget it would be an impressive way to make deletion slower without making it more correct.

## Grouped HDL deletion UX

The frontend groups a main HDL partition with subs whose `main_lba` points to it. The main row exposes the logical total size; child extents remain collapsible detail.

That same main LBA is the removal target. User-facing deletion therefore means "remove this HDL game", while the planner handles main/sub APA mechanics. Orphan subs remain diagnostic and are never attached to the nearest game by visual proximity.

After a successful known removal, `ManagementModel::apply_partition_removal()` and the session snapshot can remove affected rows in memory while preserving unrelated HDL enrichment. A full HDL metadata rebuild is not a mandatory side effect of deleting one known group.

## Recovery and persistence limits

Image mutation has deterministic in-process rollback/readback coverage and selected metadata operations can use the DriveForge Mutation Journal.

There is not yet a universal persistent crash journal that wraps an arbitrarily large multi-file PFS batch as one all-or-nothing filesystem transaction. That is an important distinction before physical-disk writes are enabled. A 20-file import whose first 19 files are individually verified is not the same semantic promise as a journaled filesystem transaction spanning all 20.

For physical-write graduation, interruption behavior, cache flushes, device identity, Windows locking and persistent recovery artifacts must be validated as their own system.

## Developer surface

`ps2-driveforge-pfs-tools` remains image-only and requires explicit `--apply` for mutations. Current developer commands cover directory/file work and APA removal planning/apply.

The exact CLI is developer surface rather than long-term UX. Production frontends should present operations and plans instead of teaching users to enjoy inode allocation syntax.

The normal DriveForge `PhysicalDrive` backend remains read-only.
