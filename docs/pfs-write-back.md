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
| per file | whole-file copy-on-write | old content stays valid until the directory entry is republished; cold-reader verification inside the writer |
| after a batch | none | session caches cleared; overlay entries retired; journal `C` marks |

Explorer sees its own changes immediately: lookups, listings and reads are served from the overlay until the commit lands.

## Failsafes

- **Backpressure:** pending data is capped (`max_pending_bytes`, default 512 MiB; `max_pending_ops`, 4096). Writes beyond it return `STATUS_DEVICE_BUSY` instead of growing an unbounded queue.
- **Free-space reserve:** PFS is never filled past `free_space_reserve_bytes` (default 64 MiB), checked at write time and again at commit. Free space comes from the allocation bitmaps, not from the superblock counter.
- **Whole-file limit:** `max_file_bytes` (default 1 GiB) per committed file.
- **Protected partitions:** `__system`, `__sysconf`, `__net` and `__mbr` are read-only unless explicitly allowed (`--allow`).
- **Circuit breaker:** after `breaker_failed_batches` (default 3) consecutive failed batches, the engine turns read-only, stops advertising uncommitted changes and stops touching the disk. Spool data and `W` intents are kept.
- **Crash recovery:** `spool/journal.log` records `W/D/M/R` intents and `C/F` outcomes. `PfsWriteBack::scan_journal()` lists uncommitted writes; `--replay-journal` re-queues them on the next mount.
- **Clean refusal:** case-only renames (`foo` → `Foo`) are refused because PFS is case-sensitive and Windows is not; cross-partition moves return `STATUS_NOT_SAME_DEVICE`.

## Using it

```text
PS2-DriveForge-Mount.exe --image ps2.img --mount P: --writable
PS2-DriveForge-Mount.exe --image ps2.img --mount P: --writable --allow +OPL --allow __common
PS2-DriveForge-Mount.exe --image ps2.img --mount P: --writable --replay-journal
```

Physical drives remain read-only in the mount until each commit batch is wired through the guarded physical lease (admission, HDDMBR backup, short RW lease, cold reopen), as already used by game deploy and library sync.

## Tests

`ps2-driveforge-pfs-write-back-tests` uses a real sparse image file shared by the read session and every writer, and proves: 600 Explorer-style copies cause zero disk sessions until commit and then land in a handful of batches byte-exact; coalescing semantics; partial overwrite seeding; copy-on-write rename; reserve, backpressure, size cap and protected partitions; breaker trip plus journal replay on the next mount; debounced background commit; and the Windows-path overlay. It is clean under ASan/UBSan and ThreadSanitizer.
