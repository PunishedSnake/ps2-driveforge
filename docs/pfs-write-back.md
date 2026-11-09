# PFS write-back engine (writable Explorer mount)

DriveForge's PFS reader and writer are native code. This document describes the layer between Windows and that writer, which is what turns the read-only Dokany mount into a writable one without the failure mode of older PC PFS tools.

## Why older tools make drives "unresponsive"

PC tools derived from the PS2 IOP drivers execute every application `write()` as an on-disk PFS operation: payload, bitmap, inode, directory entry, flush. Explorer copies issue thousands of 4-64 KiB writes plus attribute/timestamp updates, so a folder of artwork becomes tens of thousands of synchronous seeks and flushes on an IDE/USB bridge that often has no write cache. Callbacks block on the disk, Windows' I/O timeouts expire, and the device is reset or reported as unresponsive. A crash mid-sequence leaves half-published metadata.

## What DriveForge does instead

| Stage | Disk I/O | Notes |
|---|---|---|
| `CreateFile` / `WriteFile` / `SetEndOfFile` | none | data goes to a host spool file per handle; handles open lazily on the first data write |
| `Cleanup` (handle closed) | none | one pending op per path; journal `W` intent written |
| coalescing | none | last write wins; write+delete of a new file, and mkdir+rmdir, vanish |
| committer thread | batched | debounced (750 ms quiet), ordered by intent sequence, up to 256 ops per batch, one `pfs::ImageWriter` session per partition per batch, 40 ms pause between batches |
| per file | whole-file copy-on-write | committed straight from a memory-mapped spool (no file-sized heap buffer); old content stays valid until the directory entry is republished; cold-reader verification inside the writer |
| case-only rename | one sector | `foo` → `Foo` rewrites the existing dentry in place (same length) and commits before anything else in its batch |
| after a batch | none | session caches cleared; overlay entries retired; journal `C` marks |

Explorer sees its own changes immediately: lookups, listings and reads are served from the overlay until the commit lands.

## Failsafes

- **Backpressure:** pending data is capped (`max_pending_bytes`, default 512 MiB; `max_pending_ops`, 4096). Writes beyond it return `STATUS_DEVICE_BUSY` instead of growing an unbounded queue.
- **Free-space reserve:** PFS is never filled past `free_space_reserve_bytes` (default 64 MiB), checked at write time and again at commit. Free space comes from the allocation bitmaps, not from the superblock counter.
- **Whole-file limit:** `max_file_bytes` (default 4 GiB) per committed file. Because commits memory-map the spool, this limits PFS file size, not RAM.
- **Protected partitions:** `__system`, `__sysconf`, `__net` and `__mbr` are read-only unless explicitly allowed (`--allow`).
- **Circuit breaker:** after `breaker_failed_batches` (default 3) consecutive failed batches, the engine turns read-only, stops advertising uncommitted changes and stops touching the disk. Spool data and `W` intents are kept.
- **Crash recovery:** `spool/journal.log` records `W/D/M/R` intents and `C/F` outcomes. `PfsWriteBack::scan_journal()` lists uncommitted writes; `--replay-journal` re-queues them on the next mount.
- **Case-only renames:** PFS is case-sensitive and Windows is not, so both spellings share one overlay key. A dedicated `rename_case` op rewrites the dentry in place. Changes already queued inside a renamed folder are rebased to the new spelling, and new requests that still use the old spelling are rebased too. Disk reads keep the old spelling until the rename commits.
- **Clean refusal:** cross-partition moves return `STATUS_NOT_SAME_DEVICE`; editing an entry while its own case rename is queued returns `STATUS_DEVICE_BUSY` for a moment.

## Frontends

- `PS2-DriveForge-Mount.exe --writable` (images; physical with `--confirm`) publishes a one-second heartbeat to `<spool>\status.txt`.
- WinUI's **Mount** page has an *Allow writes from Explorer* toggle (physical drives go through the literal confirmation dialog) and shows the heartbeat: pending changes, commits, coalescing, failures and breaker state.

## Using it

```text
PS2-DriveForge-Mount.exe --image ps2.img --mount P: --writable
PS2-DriveForge-Mount.exe --image ps2.img --mount P: --writable --allow +OPL --allow __common
PS2-DriveForge-Mount.exe --image ps2.img --mount P: --writable --replay-journal
```

```text
PS2-DriveForge-Mount.exe --physical 2 --mount P: --writable --confirm PhysicalDrive2
```

Physical writable mounts (`make_physical_mount_writer`) follow the same guarded model as game deploy and library sync:

- **Once, at mount:** read-only admission, an APA layout fingerprint, and an FHDB-compatible HDDMBR backup under `%LOCALAPPDATA%\PS2 DriveForge\Recovery`.
- **Before every batch:** a fresh read-only open must reproduce the exact fingerprint (device size plus every APA header's id, type, start and length; PFS writes can never change it), then a fresh admission. The mount-time identity snapshot cannot be reused, because its sample windows may contain data committed by the previous batch.
- **Per batch:** a short-lived `WritablePhysicalDrive` lease with volume locks, released as soon as the batch ends. A failed check fails the batch and feeds the circuit breaker; a renumbered or repartitioned disk is never written.

## Tests

`ps2-driveforge-pfs-write-back-tests` uses a real sparse image file shared by the read session and every writer, and proves: 600 Explorer-style copies cause zero disk sessions until commit and then land in a handful of batches byte-exact; coalescing semantics; partial overwrite seeding; copy-on-write rename; reserve, backpressure, size cap and protected partitions; breaker trip plus journal replay on the next mount; debounced background commit; and the Windows-path overlay. It is clean under ASan/UBSan and ThreadSanitizer.
