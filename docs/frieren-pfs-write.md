# Frieren native PFS write path

Frieren 0.6 introduces the first native DriveForge PFS mutation layer. The goal is not to wrap `pfsshell`, mount the filesystem through FUSE, or replay one tiny command at a time. DriveForge already owns the APA/PFS parsers, so the write path stays in-process and works from validated on-disk structures directly.

This document describes the current image-only milestone. It is not permission to enable physical HDD writes yet.

## Performance model

The writer is deliberately shaped around the costs that make older host workflows unpleasant on large PS2 disks:

- one PFS probe is retained for the lifetime of an `ImageWriter` session;
- PFS bitmap metadata is loaded lazily in 1024-byte chunks and cached by `(subpart, chunk)`;
- allocating another small OPL asset normally reuses the bitmap chunk already in memory instead of rescanning all free space;
- payload data is transferred sequentially in 256 KiB batches by default instead of one sector or one shell operation at a time;
- metadata changes are narrow transactions with before-images, durable flush and byte readback;
- replacement is copy-on-write, so the existing file stays valid until the replacement payload has been written and read back;
- multi-file imports reuse one writer session through `pfs::write_batch()` instead of probing/mounting PFS once per file;
- the normal read-only PFS parser is reused for post-commit verification instead of maintaining a second permissive interpretation of the format.

These are architectural advantages, not yet benchmark claims. Release notes must not state that DriveForge is N times faster than HDL Batch Installer, PFSShell or pfsfuse until the same disk/image and host have been measured with all tools.

## File creation

The current bounded create path is:

1. Resolve the existing parent directory through the normal PFS reader.
2. Find reusable directory-entry space before reserving anything.
3. Find one contiguous run containing an inode zone plus the required data zones.
4. Mark those zones allocated in the lazily cached bitmap and commit/read-back the bitmap transaction.
5. Stream the payload in large batches and flush it.
6. Read the payload back byte-for-byte.
7. Publish a checksummed SEGD inode through `WriteTransaction`.
8. Publish the directory entry through a separate sector transaction.
9. Reopen through the normal PFS reader, resolve the path and compare the complete file.

A failure before directory-entry publication cannot expose a half-written file by name. When cleanup of abandoned allocated zones fails, the safe failure mode is leaked free space rather than a live directory entry pointing to unverified data.

## File replacement

Existing regular files use copy-on-write:

1. Validate that every old direct data extent is still marked allocated.
2. Reserve a new run without changing the live inode.
3. Write, flush and verify the replacement payload.
4. Switch the existing inode to the new extent in one transaction.
5. Verify the path and payload through a fresh PFS reader during the transaction verifier.
6. Only then release old data zones.

If the final old-zone release fails, the replacement remains valid and DriveForge reports a space-leak warning. It never frees the old extent before the inode has stopped referencing it.

## Recursive directory creation

`ImageWriter::ensure_directory()` now creates missing directory components recursively while preserving one validated PFS probe and the same lazy bitmap cache used by file writes.

For every new directory component it:

1. validates the existing parent through the normal PFS reader;
2. plans reusable parent dentry space before reserving zones;
3. reserves two contiguous zones, one for the directory SEGD inode and one for its first data block;
4. initializes a 512-byte directory sector containing verified `.` and `..` entries;
5. creates a checksummed directory inode with the normal PFS directory mode/attributes;
6. publishes the new inode and its parent dentry together in one metadata transaction;
7. resolves the new path through a fresh reader and verifies both dot entries before reporting success.

Calling `ensure_directory()` on an already-existing directory tree is a zero-mutation no-op. Encountering a regular file where a directory component is required is a hard refusal.

Directory *growth* is deliberately separate from directory creation. When an existing directory has no reusable dentry slack, Frieren still refuses rather than silently inventing a new layout. That next step will extend direct directory data safely before SEGI support is introduced.

## Batch writes and OPL asset import

`pfs::write_batch()` executes many regular-file operations through one `ImageWriter` session. Each file keeps its own safe publication sequence, so the batch does not require a filesystem-sized before-image in memory. Required failures stop the batch by default; optional failures can be reported without discarding already verified required work.

The host-side `opl_pfs_import` module is intentionally separate from both HTTP fetching and raw PFS format code:

```text
provider fetch/staging
 -> immutable PFS import preflight
 -> normalize/deduplicate exact target paths
 -> derive parent directories shallow-to-deep
 -> freeze staged host bytes before mutation
 -> ensure required directories through ImageWriter
 -> one cached pfs::write_batch session
 -> normal-reader verification per file
```

Cache-only provider data is not written to PFS. HDD-OSD metadata is reported as a separate placement domain. TAR members fail closed until DriveForge has a real TAR-container updater; the importer never pretends an archive member is an ordinary PFS file.

## PC partition-map safety gate

Frieren now has a read-only `disk_layout_guard` module for the conventional PC partitioning sectors. It distinguishes ordinary legacy MBR evidence from protective GPT, hybrid GPT/MBR and an orphan GPT header. Protective/hybrid/GPT evidence is a hard mutation refusal in the image-only PFS developer CLI.

This guard does not replace APA validation. A future physical writer must pass both gates: no conflicting PC GPT ownership and a clean, positively identified PS2 APA disk.

## Remaining bounded limits

The writer still refuses rather than improvises when:

- an existing directory has no reusable dentry slack and must grow;
- a target requires indirect SEGI descriptors;
- a file cannot currently be represented by one contiguous new data run;
- a TAR-container destination must be updated;
- the PFS probe or APA bounds are not clean.

Next PFS milestones are direct directory growth, fragmented direct extents, SEGI creation, deterministic TAR updates and stronger persistent recovery/journal semantics before any physical-disk writer is enabled.

## Fast APA partition removal

Deleting an APA partition is intentionally independent from PFS file deletion and from HDL catalog enrichment. An APA partition becomes free when its headers are no longer reachable from the APA linked list. The payload does not need to be zero-filled.

`plan_remove_main_partition()` therefore validates the clean chain, collects the selected main header and all of its declared sub headers, and computes only the surviving link rewrites. `remove_main_partition_from_image()` stages those small rewrites through `WriteTransaction` and verifies the resulting chain with a fresh APA scan.

Detached headers and payload sectors are left physically untouched. They are unreachable free space and may be overwritten by a future allocation. This keeps the mutation proportional to the number of neighbouring APA headers whose links actually change, rather than proportional to game size.

## No giant HDD-list rebuild after delete

The user-facing HDD manager is already built from `PartitionCatalog`, which itself is a pure in-memory transform of a validated APA scan. HDL title/startup enrichment is deliberately lazy and separate.

After a successful removal, `ManagementModel::apply_partition_removal()` deletes the affected main/sub rows from the existing model and rebuilds only its small LBA-to-row index and counters in RAM. Surviving parsed HDL metadata stays attached to its rows. A late background enrichment result for a removed LBA is ignored.

This means the intended GUI delete path is:

1. use the session's validated APA snapshot to make a removal plan;
2. commit the small link transaction;
3. apply that exact plan to the in-memory manager/session snapshot;
4. optionally schedule a low-priority consistency rescan later, but never block the UI on a full HDL metadata rebuild.

The conservative developer CLI currently performs a post-delete APA rescan because it has no persistent GUI model to update. The GUI path should not confuse that verification policy with a requirement to rediscover every game.

## Developer surface

`ps2-driveforge-pfs-tools` is image-only and requires explicit `--apply` for mutations:

```text
ps2-driveforge-pfs-tools mkdir <disk-image> <pfs-partition-id> <pfs-directory> --apply
ps2-driveforge-pfs-tools put <disk-image> <pfs-partition-id> <host-file> <pfs-path> --apply
ps2-driveforge-pfs-tools plan-remove <disk-image> <partition-id|lba>
ps2-driveforge-pfs-tools remove-partition <disk-image> <partition-id|lba> --apply
```

`put` automatically ensures missing parent directories first. The tool reports mutation/verification time, payload write calls and metadata/bitmap work so later comparative benchmarks can measure the actual implementation instead of guessing from wall-clock folklore.

The regular DriveForge `PhysicalDrive` backend remains read-only in this milestone.
