# Frieren 0.6.0 plan

Frieren is the first DriveForge release train allowed to mutate PS2 storage. The Emilia read stack remains structurally read-only; Frieren adds separate writable capabilities with explicit admission, recovery artifacts and post-write verification.

This is the implementation-state document for the current `feat/frieren-hdl-write-foundation` branch. A milestone marked implemented may still be release-blocked on real-hardware validation.

## Non-negotiable capability boundary

`BlockDevice` stays read-only. The ordinary Windows `PhysicalDrive` continues to use read-only access and never gains a write method.

Mutation requires the separate `WritableBlockDevice` capability. Physical mutation uses `WritablePhysicalDrive` only after a read-only preflight has authorized the exact PS2 disk identity and the caller has explicitly opted into a destructive operation.

Normal mutation and exceptional recovery are separate authorization domains. A PFS/HDL operation does not gain permission to repair APA metadata merely because both ultimately write sectors.

## F1 - bounded HDL metadata writes - implemented

`hdl::patch_game_metadata()` updates selected fields inside an already valid HDL metadata block. The operation captures a before-image, writes the bounded range, flushes, reads back, reparses through the normal HDL reader and rolls back on verification failure.

The normal parser remains the post-write judge. Writers do not grade their own homework with a more forgiving reader.

## F2 - mutation transactions and journal - implemented

`WriteTransaction` provides sector-aligned before-images, ordered writes, durable flush/readback verification and rollback.

The DriveForge-private `PS2DFRC1` Mutation Journal has the lifecycle:

```text
PREPARED -> COMMITTED
         -> RESTORED
```

The Mutation Journal is transaction machinery. It is deliberately distinct from the FHDB Rescue Capsule and shared FHDB recovery formats.

## F3 - APA allocation and removal planning - implemented

Pure planners cover HDL allocation and main-partition removal before mutation. APA publication validates chain relationships, bounds, main/sub limits and stale neighbour state.

Deletion operates on a main LBA plus its authoritative owned sub headers. One HDL game is therefore one logical deletion unit.

## F4 - complete HDL install - image and guarded physical paths implemented

The storage-neutral install pipeline is:

```text
ISO
 -> ISO9660 / SYSTEM.CNF startup validation
 -> zero-write HDL allocation plan
 -> APA header + DEADFEED metadata generation
 -> payload stream into unpublished free extents
 -> payload readback verification
 -> durable flush
 -> metadata + APA publication transaction
 -> cold normal APA + HDL parser verification
```

APA visibility is published only after payload verification. Interrupted payload copies may leave unreachable bytes in free space, but not a published half-game.

Image mutation uses `WritableFileBlockDevice`. Physical mutation uses the separate guarded physical writer after read-only admission and exact device confirmation.

## F4a - OPL Asset Pipeline - implemented

Provider planning/staging uses the ISO startup ID as the canonical key and supports title/Redump metadata, CFG, compatibility overlay, widescreen CHT, bare-mastercode fallback, artwork and optional HDD-OSD data.

Network data is staged and validated on the host before any filesystem mutation. Network bytes never flow directly into raw-disk writes.

See [`opl-asset-pipeline.md`](opl-asset-pipeline.md).

## F4b - native PFS mutation - implemented for Frieren scope

Current native writer coverage includes:

- writable APA volume translation;
- bitmap/extent allocation;
- regular file create and replacement;
- copy-on-write replacement safety;
- directory creation and growth;
- SEGI and fragmented cases;
- tree removal;
- batch operations;
- OPL loose-file import;
- TAR archive destination handling;
- cold reader verification after writes.

See [`frieren-pfs-write.md`](frieren-pfs-write.md).

## F5 - HDL delete and grouped management - core implemented, GUI integration active

APA removal planning and apply are implemented for images and guarded physical media. Fast deletion makes payload/header extents unreachable free space rather than zero-filling gigabytes.

`ManagementModel` can consume a committed removal without rebuilding the full HDL list. Presentation groups a main and its authoritative subs through `main_lba`, exposes logical total size and leaves orphan subs diagnostic.

The remaining work is the first-class HDL Tools GUI surface.

## F5R - FHDB Manager recovery/rescue parity - implemented in core and physical developer tooling

Implemented shared recovery formats and behaviors include:

- `PS2HBRC\0` v1 Rescue Capsule parsing/building/validation;
- `HDDRESCUE.BIN` / `HDDRESCUE2.BIN` two-slot policy;
- `HDDMBR.BIN` / `HDDMBR2.BIN` exact master backups;
- legacy `FHDBMBR.BIN` / `FHDBMBR2.BIN` restore input;
- `HDDRAW.BIN` / `HDDRAW2.BIN` exceptional master snapshots;
- `APAMETA1` `HDDMETA.BIN` / `HDDMETA2.BIN` forensic touched-header snapshots;
- `FORENSIC.TXT` report export;
- raw APA forensic discovery and conservative repair planning;
- Rescue Capsule capture from the exact published `osdStart` / `osdSize` payload;
- full bootstrap restore using payload-first / pointer-last publication;
- legacy pointer-only restore;
- physical exceptional recovery admission and guarded repair endpoints.

A corrupt or wrong-disk full Rescue Capsule blocks fallback to an older legacy pointer. A valid header-only capsule may permit fallback according to FHDB Manager precedence.

`PS2DFRC1` remains a different wire format with a different trust domain.

The authoritative parity contract is [`fhdb-bootstrap-parity.md`](fhdb-bootstrap-parity.md).

## F6 - guarded physical-disk mutation - implemented, hardware-gated

The physical capability is no longer hypothetical.

Frieren now contains a Windows-only `WritablePhysicalDrive` path with:

- `GENERIC_READ | GENERIC_WRITE` on the short-lived writable handle only;
- mandatory 512-byte logical sectors;
- target-volume locking/dismount where applicable;
- write-through and explicit flush semantics;
- a frozen SHA-256 media identity built from device size and widely separated sample windows;
- repeated admission checks before commit;
- normal mutation admission requiring GPT-clear, clean canonical APA and matching identity;
- exceptional recovery admission requiring GPT-clear and matching identity while allowing the recovery planner to reason about damaged APA bytes;
- mandatory recovery/safety artifacts appropriate to the operation;
- parser/readback verification and cold read-only reopen after physical mutation.

The Windows developer surface is `ps2-driveforge-physical-tools` and currently exposes:

```text
preflight
forensic-scan
capture-rescue
install-hdl
remove
restore-bootstrap
repair-master
repair-forensic
```

Destructive commands require both `--apply` and literal `--confirm PhysicalDriveN`.

**Release status:** implemented but not yet graduated. Sacrificial real-HDD destructive validation, failure/recovery drills and cross-tool recovery round trips remain release blockers.

## F7 - HDL Tools GUI - active integration

The frontend must expose operations rather than raw sectors:

- install a game from ISO;
- show startup ID, title and install plan before mutation;
- optional OPL metadata/artwork/fixes;
- provider and destination preview;
- edit title/compatibility/DMA metadata;
- delete one grouped HDL game;
- show total allocated game size including subs;
- expose subpartitions in a collapsible diagnostic view;
- preview affected ranges and verification outcome;
- create/restore Rescue Capsules;
- expose bootstrap and forensic recovery using FHDB terminology;
- make physical destructive actions visibly different from image mutation and require explicit confirmation.

The management surface is **HDL Tools**, not another passive HDL Games list.

## F8 - MagicGate host service and bootstrap provider completion - active

The host-side MagicGate work is part of Frieren's remaining implementation workload.

Current branch contains the crypto/content primitives, known-vector tests and a dedicated Linux/Windows `MagicGate host verification` workflow. Completion requires turning those primitives into one validated service boundary used by the bootstrap provider pipeline rather than leaving callers to assemble cryptographic steps ad hoc.

The completed path must enforce:

```text
bounded provider acquisition
 -> pinned provenance / SHA-256
 -> staged content inspection
 -> MagicGate/KELF validation or transformation through the host service
 -> immutable bootstrap install plan
 -> recovery artifacts
 -> guarded image/physical write endpoint
 -> cold verification
```

No network provider is allowed to hand bytes directly to a raw writer. Key material and signing/encryption state must use explicit typed inputs and fail closed when unavailable or inconsistent.

## Recovery UX vocabulary

DriveForge and FHDB Manager should present the same operation names where semantics overlap:

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

Platform UI may differ. Artifact meaning must not.

## Frieren release gate

0.6.0 is not releasable until all of the following are true:

- Emilia read-only regressions remain green;
- writable image tests remain green on MSVC and Linux sanitizers;
- MagicGate host verification is green on Linux and Windows;
- canonical Windows package verification includes every Frieren regression/developer executable required by the release;
- HDL install/delete and PFS mutations cold-reopen cleanly;
- corruption/refusal tests prove malformed layouts are not casually mutated;
- Rescue Capsule, Mutation Journal and FHDB shared artifacts remain distinct and cross-format rejection is tested;
- recovery restore precedence matches FHDB Manager;
- wrong-disk and stale-plan cases fail before target writes;
- guarded physical HDL install/delete are validated on a disposable PS2 HDD;
- physical recovery and deliberate failure/recovery drills are validated on sacrificial media;
- shared recovery artifacts round-trip FHDB Manager -> DriveForge and DriveForge -> FHDB Manager on representative real samples;
- HDL Tools GUI exposes the supported mutation/recovery flows without bypassing the guarded backend;
- the MagicGate host-service/provider path is complete, typed, tested and integrated with bootstrap planning;
- a final exact candidate artifact passes the Frieren destructive-hardware checklist.

The rule remains pleasantly unambitious: read paths stay boring, write paths stay explicit, recovery stays interoperable, cryptography fails closed and no button earns access to a real disk merely because a synthetic image survived human enthusiasm.
