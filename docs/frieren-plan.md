# Frieren 0.6.0 plan

Frieren is the first DriveForge release train allowed to mutate PS2 storage. The goal is not to make the existing read-only stack casually writable. The goal is to add narrowly scoped write capabilities, prove them on disposable images, and only then graduate to real disks.

## Non-negotiable write boundary

`BlockDevice` stays read-only. Mutation requires the separate `WritableBlockDevice` capability.

The first writable backend is `WritableFileBlockDevice`, which opens an existing image read/write, never creates or truncates it, never extends it, and exposes an explicit durability `flush()` operation. `PhysicalDrive` remains `GENERIC_READ` during the first Frieren milestones.

This keeps every existing Emilia read-only code path read-only by type. A parser, browser, Dokany mount, or session cannot accidentally write merely because the source backend later gains a writable sibling.

## Milestone F1: bounded HDL metadata writes

The first real write is deliberately non-structural.

`hdl::patch_game_metadata()` may update only these fields inside an already valid native HDL header at main-partition `+0x101000`:

- title;
- compatibility flags;
- DMA mode.

It does not modify:

- APA headers or linked-list pointers;
- main/sub partition extents;
- HDL allocation-table entries;
- startup ID;
- media type or layer break;
- game payload sectors.

The operation validates the existing header with the normal reader, captures the complete 1536-byte metadata before-image, performs one fixed-size write, flushes it, reads it back byte-for-byte, reparses it through the normal HDL reader, and rolls the original metadata back if verification fails.

F1 is image-only until its portable and Windows regression tests are green.

## Milestone F2: mutation transaction primitives

Before APA layout changes, add reusable transaction support for sector-aligned before-images and an explicit commit record. Required properties:

1. every mutable range is known before the first write;
2. original bytes can be exported before commit;
3. verification reads use the same parser paths used by normal browsing;
4. interrupted or failed commits have a deterministic recovery story;
5. read caches are invalidated only after a successful commit.

The transaction layer must not invent APA rules. It only records, writes, flushes, verifies, and restores ranges supplied by format code.

## Milestone F3: APA allocation planner

Implement a pure, zero-write planner first. Given one validated APA scan and a requested HDL payload size it must produce a proposed main/sub allocation without mutating the source.

The planner must validate:

- free-space extents and alignment rules;
- linked-list predecessor/successor updates;
- main/sub limits;
- device bounds and integer overflow;
- the resulting chain as if it had already been written.

Only after synthetic-image tests prove the plan should a separate commit function serialize APA headers.

## Milestone F4: HDL install to image

The first complete game install remains image-only.

Expected flow:

```text
ISO/source image
 -> validate ISO9660 / startup ID / media geometry
 -> compute required HDL allocation
 -> build APA mutation plan
 -> write game payload into planned extents
 -> write native HDL metadata/allocation table
 -> commit APA headers last
 -> flush
 -> cold re-open and full APA + HDL verification
```

APA visibility should be the final commit step so an interrupted payload copy does not publish a half-installed game as a valid partition.

## Milestone F5: HDL delete from image

Deletion gets its own planner and recovery tests. It must remove one selected HDL main partition plus its owned subpartitions, repair the APA chain, and leave unrelated partitions byte-identical.

No UI delete button ships before synthetic tests cover first/middle/last partitions, fragmented layouts, maximum sub counts, corrupt-chain refusal, and rollback failure reporting.

## Milestone F6: real physical-disk writes

Only after image install/delete is boringly repeatable do we add a writable raw-disk backend.

Physical writes require an explicitly separate open mode/backend and additional gates:

- administrator access;
- positive PS2 APA identification;
- source identity/capacity re-check immediately before commit;
- volume/disk locking where Windows permits it;
- mandatory backup/recovery artifact for metadata ranges;
- explicit user confirmation naming the target disk;
- write-through/flush semantics;
- cold re-open verification after commit.

The existing `PhysicalDrive` remains read-only even after this exists.

## Milestone F7: HDL Tools UI

The WinUI page should expose operations, not raw sector editing:

- install game;
- edit title/compatibility/DMA metadata;
- delete game;
- show planned space usage and affected partitions;
- export recovery metadata before destructive commits;
- show a detailed verification result after each operation.

The old "HDL Games" concept becomes **HDL Tools** because the page owns management actions, not merely another view of the game list.

## Frieren release gate

0.6.0 is not releasable until:

- all existing Emilia read-only tests remain green;
- writable image backend tests are green on MSVC and Linux sanitizers;
- HDL metadata patch/rollback tests are green;
- install/delete image tests include cold re-open verification;
- corruption/refusal tests prove malformed APA/HDL layouts are not mutated;
- at least one disposable real PS2 HDD has completed install, boot/read validation on console, delete, and post-delete APA verification;
- recovery artifacts have been exercised rather than merely generated.

The guiding rule is simple: read-only code stays boring, write code stays explicit, and no operation earns a real-disk button merely because it survived one lucky test image.
