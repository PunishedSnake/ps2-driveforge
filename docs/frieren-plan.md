# Frieren 0.6.0 plan

Frieren is the first DriveForge release train allowed to mutate PS2 storage. The goal is not to make the Emilia read stack casually writable. The goal is to add narrow capabilities, prove them on disposable images, make recovery interoperable with FHDB Manager, and only then consider real disks.

This is a living plan. Completed milestones are marked accordingly so it does not continue predicting features that are already sitting in CMake, which is a surprisingly common genre of documentation.

## Non-negotiable capability boundary

`BlockDevice` stays read-only. Mutation requires the separate `WritableBlockDevice` capability.

`WritableFileBlockDevice` opens an existing image read/write, never creates or truncates it, never extends it and exposes explicit `flush()`. The existing Windows `PhysicalDrive` remains `GENERIC_READ`.

Normal mutation and exceptional recovery are also separate authorization domains. Both may eventually write through the same low-level capability, but a PFS file operation does not acquire permission to repair sector zero merely through proximity.

## F1 - bounded HDL metadata writes - implemented

`hdl::patch_game_metadata()` updates selected fields inside an already valid HDL metadata block. The operation captures the complete metadata before-image, writes a fixed range, flushes, reads back, reparses through the normal HDL reader and rolls back on verification failure.

This established the first rule of Frieren writes: the normal parser is the post-write judge. Writers do not get to grade their own homework with a more forgiving reader.

## F2 - mutation transactions and journal - implemented

`WriteTransaction` provides sector-aligned before-images, ordered writes, flush/readback, parser verification and rollback.

`Mutation Journal` adds the durable `PS2DFRC1` sidecar lifecycle for selected metadata transactions:

```text
PREPARED -> COMMITTED
         -> RESTORED
```

The Mutation Journal is DriveForge-private transaction machinery. It is not the FHDB Rescue Capsule and never uses that name. See [`fhdb-bootstrap-parity.md`](fhdb-bootstrap-parity.md).

## F3 - APA allocation and removal planning - implemented

Pure planners now cover HDL allocation and main-partition removal before mutation. APA publication validates chain relationships, device bounds, main/sub limits and stale neighbour state.

Deletion operates on a main LBA plus all authoritative owned sub headers. The UI can therefore present one HDL game as one deletion unit rather than asking the user to manually interpret a bouquet of unnamed partitions.

## F4 - complete HDL install to image - implemented

The image-only install flow is:

```text
ISO
 -> ISO9660 / SYSTEM.CNF startup validation
 -> zero-write HDL allocation plan
 -> APA header + DEADFEED metadata generation
 -> payload stream into unpublished free extents
 -> byte readback of payload
 -> durable flush
 -> metadata + APA publication transaction
 -> normal APA + HDL parser verification
```

APA visibility is published after payload verification. An interrupted copy can leave orphan payload bytes in free space, but it does not expose a half-installed game as valid.

The installer can optionally protect its small publication transaction with a **Mutation Journal**. Bulk ISO payload is deliberately outside that journal because before-imaging gigabytes of free space would turn a metadata safety feature into a second disk image.

## F4a - OPL Asset Pipeline - implemented

Provider planning/staging uses the ISO startup ID as the canonical key and supports title/Redump metadata, CFG, compatibility overlay, widescreen CHT, bare-mastercode fallback, artwork and optional HDD-OSD data.

Network data is staged on the host with provenance and response/path validation before any filesystem mutation. The downloader has no direct disk writer because HTTP has already been granted enough influence over civilization.

See [`opl-asset-pipeline.md`](opl-asset-pipeline.md).

## F4b - native PFS asset import - implemented for Frieren scope

The branch now contains native image-only PFS mutation rather than the earlier read-only placeholder described by the first draft of this plan.

Current writer work includes:

- writable APA volume translation;
- bitmap/extent allocation;
- regular file create and replacement;
- copy-on-write replacement safety;
- directory creation/growth support;
- SEGI/fragmented cases;
- tree removal;
- batch operations;
- OPL loose-file import;
- TAR archive destination handling;
- cold reader verification after writes.

Existing user CFG content remains merge input where applicable. The writer and normal reader share on-disk semantics, not a private post-write interpretation.

See [`frieren-pfs-write.md`](frieren-pfs-write.md).

## F5 - HDL delete and grouped UX - core implemented, frontend integration continuing

APA removal planning and image apply are implemented. `ManagementModel` can consume a committed removal without rebuilding every unrelated HDL result.

Partition presentation groups a main and its authoritative subs through `main_lba`, exposes a logical total size and keeps orphan subs diagnostic rather than assigning them by adjacency.

The remaining frontend task is to make the grouped HDL game action the obvious delete UX while keeping physical-drive mutation unavailable.

## F5R - FHDB Manager recovery/rescue parity - image layer implemented

Recovery is now an explicit Frieren milestone because write support without an interoperable recovery story is merely confidence with better branding.

Implemented shared formats and behaviors include:

- `PS2HBRC\0` v1 FHDB Rescue Capsule parsing/building/validation;
- `HDDRESCUE.BIN` / `HDDRESCUE2.BIN` two-slot policy;
- `HDDMBR.BIN` / `HDDMBR2.BIN` exact master backups;
- legacy `FHDBMBR.BIN` / `FHDBMBR2.BIN` restore input;
- `HDDRAW.BIN` / `HDDRAW2.BIN` exceptional master snapshots;
- `APAMETA1` `HDDMETA.BIN` / `HDDMETA2.BIN` forensic touched-header snapshots;
- `FORENSIC.TXT` report export;
- raw APA forensic discovery and conservative repair planning;
- image-only single-master and topology repair apply;
- image-only full Rescue Capsule bootstrap restore;
- image-only legacy pointer-only restore;
- same-disk, stale-plan, live-bounds, payload-first and sector-zero-last safety rules.

Full Rescue restore mirrors FHDB Manager selection precedence. A corrupt or wrong-disk `HDDRESCUE*` blocks fallback to an older legacy pointer. A valid header-only capsule may fall back. Payload is written, flushed and byte-verified before `osdStart`/`osdSize` are published. The current master is backed up before the first target write.

`PS2DFRC1` remains a separate DriveForge Mutation Journal. A test deliberately presents its magic as `HDDRESCUE.BIN` and expects refusal. We are not allowing two recovery formats to merge into one conceptual soup merely because both contain before-images.

The authoritative parity contract is [`fhdb-bootstrap-parity.md`](fhdb-bootstrap-parity.md).

## F6 - physical-disk writes - locked

Physical writes remain a separate future capability.

Graduation requires more than changing `GENERIC_READ` to `GENERIC_READ | GENERIC_WRITE` and hoping the review is distracted. Required gates include:

- administrator access and exact device identity;
- positive PS2 APA identification;
- immediate source identity recheck before commit;
- Windows disk/volume locking where meaningful;
- explicit target naming and confirmation;
- mandatory mutation or FHDB recovery artifact appropriate to the operation;
- write-through/flush semantics;
- cold reopen verification;
- fault injection;
- sacrificial HDD validation;
- cross-platform FHDB artifact round trips.

The existing `PhysicalDrive` remains read-only even after a future writable physical backend exists.

## F7 - HDL Tools UI - active integration

The frontend should expose operations, not raw sector editing:

- install game;
- optional OPL metadata/artwork/fixes;
- provider and destination preview;
- edit title/compatibility/DMA metadata;
- delete one grouped HDL game;
- show total allocated game size including subs;
- expose subpartitions in a collapsible diagnostic view;
- preview affected ranges and verification outcome;
- expose recovery/rescue tools with FHDB terminology, not generic "recovery file" labels.

The management surface is **HDL Tools**, not merely another HDL Games list.

## Recovery UX direction

DriveForge and FHDB Manager should feel like two platform-specific windows onto one recovery vocabulary. Where operations overlap, names and artifacts should match:

```text
Create Rescue Capsule
Restore Rescue Capsule
Restore legacy HDDMBR/FHDBMBR pointer
Inspect boot-chain evidence
Export HDDRAW
Export HDDMETA / FORENSIC report
Forensic APA analysis
Apply guarded repair
```

Platform-specific UI may differ. The meaning of the button should not.

## Frieren release gate

0.6.0 is not releasable until:

- Emilia read-only regressions remain green;
- all current writable image tests are green on MSVC and Linux sanitizers;
- package verification includes every canonical Frieren regression executable;
- HDL install/delete and PFS mutations cold-reopen cleanly;
- corruption/refusal tests prove malformed layouts are not casually mutated;
- Rescue Capsule, Mutation Journal and FHDB shared artifacts remain distinct and cross-format rejection is tested;
- recovery restore precedence matches FHDB Manager;
- wrong-disk and stale-plan restore cases fail before target writes;
- real-hardware game/PFS validation is completed on a disposable PS2 HDD;
- shared recovery artifacts are round-tripped FHDB Manager -> DriveForge and DriveForge -> FHDB Manager on representative real samples;
- physical PC writes remain disabled unless their separate gate has actually been completed.

The rule remains pleasantly unambitious: read paths stay boring, write paths stay explicit, recovery paths stay interoperable, and no button earns access to a real disk merely because one test image survived human enthusiasm.
