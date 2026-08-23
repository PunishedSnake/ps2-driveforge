# FHDB Manager parity and interchange contract

PS2 DriveForge and **FHDB Manager** are two frontends to the same PS2 HDD recovery problem. FHDB Manager runs on the console. DriveForge runs on the host PC. The repository for FHDB Manager is still named `fhdb-bootstrap-manager` for historical reasons, because repository names apparently enjoy a longer half-life than product names.

Parity therefore means **recovery capability parity, safety-policy parity and artifact interoperability**. It does not mean copying the PS2 GS renderer into Windows or teaching the console about Win32 dialogs. A backup created on one platform should be understandable on the other whenever the format has a useful cross-platform meaning, and both tools should reach the same refusal decision from the same evidence.

The rule that prevents the most expensive naming mistake is deliberately simple:

> **FHDB Rescue Capsule and DriveForge Mutation Journal are different formats with different jobs. They are never synonyms.**

## Stable vocabulary and wire identities

| Term | Wire/file identity | Purpose | Interchange |
| --- | --- | --- | --- |
| **FHDB Rescue Capsule** | `PS2HBRC\0`, v1, normally `HDDRESCUE.BIN` / `HDDRESCUE2.BIN` | Portable snapshot of the exact APA master plus the active HDD bootstrap payload and integrity metadata | **Shared by FHDB Manager and DriveForge** |
| **DriveForge Mutation Journal** | `PS2DFRC1`, v1, normally a host-side `.ps2df-journal` sidecar | Durable before/after record for an in-progress DriveForge metadata transaction and deterministic rollback | DriveForge-private; **not** a Rescue Capsule |
| APA master backup | exact 1024 bytes, `HDDMBR.BIN` / `HDDMBR2.BIN` | Normal master-header safety snapshot and legacy pointer source | Shared |
| Legacy master backup | exact 1024 bytes, `FHDBMBR.BIN` / `FHDBMBR2.BIN` | Older FHDB naming accepted during restore discovery | Shared input compatibility |
| Exceptional raw master snapshot | exact 1024 bytes, `HDDRAW.BIN` / `HDDRAW2.BIN` | Original sectors 0-1 before exceptional master repair | Shared |
| Forensic touched-header snapshot | `APAMETA1` v1, `HDDMETA.BIN` / `HDDMETA2.BIN` | Original image and SHA-256 of every APA header a topology repair may touch | Shared |
| Human forensic report | `FORENSIC.TXT` | Candidate maps, confidence, discovered headers and write-gate state | Shared schema |

`PS2DFRC1` keeps its original development wire magic so an interrupted DriveForge transaction does not become unreadable merely because the API was renamed. The semantic/API name is nevertheless **Mutation Journal** everywhere. No `RecoveryCapsule` compatibility alias exists in DriveForge. If a DriveForge API says Rescue Capsule, it means the FHDB `PS2HBRC\0` format.

Readers fail closed on the wrong format. A Mutation Journal parser rejects `PS2HBRC\0`; an FHDB Rescue Capsule parser rejects `PS2DFRC1`. There is no heuristic cross-format import. Eight magic bytes are cheap. Recovering from a tool that guessed wrong is less cheap.

## Capability matrix

| FHDB Manager capability | DriveForge status | DriveForge owner |
| --- | --- | --- |
| Validate APA `__mbr`, magic, checksum and bounds | Frieren | `apa::Reader` |
| Enumerate complete APA chain and main/sub relationships | Frieren | `apa::Reader`, `PartitionCatalog` |
| Group main + authoritative sub-partitions for UX | Frieren | `group_partition_catalog`, `ManagementModel::groups()` |
| Refuse malformed/noncanonical chains before normal mutation | Frieren | APA allocation/removal planners |
| Detect protective/hybrid GPT evidence before write | Frieren | `disk_layout_guard`, recovery admission |
| Inspect HDD OSD bootstrap pointer (`osdStart`, `osdSize`) | Frieren | FHDB master/rescue parsing |
| Header-only APA master backup | Frieren | `fhdb_artifacts`, `HDDMBR*.BIN` |
| Full Rescue Capsule v1 parse/validation | Frieren | `fhdb_rescue` |
| Rescue Capsule SHA-256 verification | Frieren | `fhdb_rescue`, `sha256` |
| Create canonical Rescue Capsule from supplied master/payload | Frieren | `fhdb_rescue` |
| Discover `HDDRESCUE*` slots with FHDB Manager precedence | Frieren image-only | `fhdb_restore` |
| Restore capsule payload before exposing pointer | Frieren image-only | `fhdb_restore` |
| Verify same-disk identity before and immediately before restore | Frieren image-only | `fhdb_restore` |
| Validate payload against live `__mbr` geometry | Frieren image-only | `fhdb_restore` |
| Save current `HDDMBR*` before bootstrap restore | Frieren image-only | `fhdb_restore`, `fhdb_artifacts` |
| Legacy `HDDMBR*` / `FHDBMBR*` pointer-only restore | Frieren image-only | `fhdb_restore` |
| Invalid/wrong-disk full rescue blocks legacy fallback | Frieren image-only | `fhdb_restore` |
| Structural KELF inspection for Rescue Capsule | Frieren | `fhdb_rescue` |
| Preserve exact raw sectors 0-1 before exceptional repair | Frieren | `fhdb_artifacts`, `HDDRAW*.BIN` |
| Raw forensic APA discovery independent of normal admission | Frieren | `apa_forensic` |
| Forward/reverse/geometry candidate maps | Frieren | `apa_forensic` |
| Conflict/overlap/dormant-free evidence classification | Frieren | `apa_forensic` |
| Truncated-scan fail-closed policy | Frieren | `apa_forensic`, recovery executor |
| Single-master narrowly reconstructed repair | Frieren image-only | `apa_repair`, `apa_recovery_apply` |
| Multi-header `prev`/`next` topology repair plan | Frieren image-only | `apa_forensic`, `apa_recovery_apply` |
| Save every touched original header before repair | Frieren | `fhdb_artifacts`, `HDDMETA*.BIN` |
| Export `FORENSIC.TXT` | Frieren | `fhdb_artifacts` |
| Commit non-master headers first, master LBA 0 last | Frieren image-only | `apa_recovery_apply` |
| Flush/read-back/final touched-set verification | Frieren | recovery executors |
| Reject stale plan when source changed | Frieren | frozen source snapshots + immediate reread |
| DriveForge durable metadata rollback journal | Frieren | `mutation_journal`, **not an FHDB artifact** |
| MagicGate signing of stock MBR KELF | Planned | separate signing provider/service boundary |
| Install replacement MBR program inside reserved `__mbr` area | Planned | bootstrap installer with reserved-range proof |
| Boot-chain evidence report | Planned parity | bootstrap evidence service |
| FMCB HDD-skip / downstream boot evidence inspection | Planned parity | host evidence scanners |
| Deterministic unified structure-health verdict | Partial | APA/PFS/forensic diagnostics |
| Read-only shadow forensic APA map browsing | Planned UI | immutable forensic snapshot |
| Hardware fault injection against images | Planned | recovery fault-injection tests |
| Hardware fault injection against sacrificial physical HDD | Planned and gated | hardware validation tooling |
| Physical recovery writes | Locked | separate future physical recovery capability |
| GS video modes/themes/fonts | PS2-only | no host parity requirement |

## FHDB Rescue Capsule v1 contract

DriveForge mirrors the FHDB Manager wire format rather than defining a suspiciously similar container with one field moved for artistic expression. A complete capsule is:

```text
0x0000   256 bytes   PS2HBRC v1 metadata
0x0100  1024 bytes   exact APA __mbr header
0x0500  variable     exact sector-aligned active bootstrap payload, if any
```

The v1 metadata records complete size, flags, `osdStart`, `osdSize`, payload bytes, SHA-256 of the master and payload, ROMVER/family/confidence strings and the unpadded KELF length when structurally valid. Reserved bytes remain zero. Unknown flags or versions are rejected rather than interpreted optimistically.

A Rescue Capsule is a **portable bootstrap recovery artifact**. It does not describe arbitrary HDL/PFS metadata transactions and has no PREPARED/COMMITTED/RESTORED lifecycle.

## DriveForge Mutation Journal contract

The Mutation Journal protects one small DriveForge metadata transaction. `PS2DFRC1` records target device size and non-overlapping sector-aligned ranges with exact before and after bytes. Its lifecycle is:

```text
PREPARED -> COMMITTED
         -> RESTORED   only when a PREPARED transaction is rolled back
```

The journal is written durably before the protected mutation. Inspection classifies the target as all-before, all-after, mixed or foreign/corrupt. A PREPARED mixed state may be restored to exact before-images. A COMMITTED journal is never automatically rolled back. This is transaction machinery, not a substitute for `HDDRAW`, `HDDMETA`, `HDDMBR` or `HDDRESCUE`.

## Bootstrap restore discovery contract

DriveForge intentionally mirrors FHDB Manager restore discovery. The order is part of interoperability, not an implementation detail:

```text
HDDRESCUE.BIN
HDDRESCUE2.BIN
    |
    +-- valid full same-disk capsule with valid KELF -> use it
    |
    +-- valid header-only capsule -> remember and continue
    |
    +-- corrupt / wrong disk / invalid payload -> block legacy fallback

Only when no invalid full capsule was seen:

HDDMBR.BIN
HDDMBR2.BIN
FHDBMBR.BIN
FHDBMBR2.BIN
    |
    +-- same-disk canonical master with nonzero pointer -> pointer-only restore
```

This matters. If `HDDRESCUE.BIN` exists but is corrupt or belongs to another disk, quietly using an older `HDDMBR.BIN` would hide the strongest evidence that the requested restore set is inconsistent. Both tools refuse instead.

Header-only Rescue Capsules are different. They represent a valid snapshot with bootstrap disabled, so they permit legacy pointer fallback exactly as FHDB Manager does.

## Full bootstrap restore ordering

For image-backed restore, DriveForge uses the same safety ordering as FHDB Manager:

1. validate the Rescue Capsule completely;
2. prove same-disk identity against the live master;
3. validate `payload_start` and `payload_sectors` against the live `__mbr` geometry and the 4 MiB FHDB limit;
4. freeze the complete current master in the restore plan;
5. immediately before apply, reread the current master and require byte-for-byte equality with the frozen source;
6. save and verify the current master as `HDDMBR.BIN` or `HDDMBR2.BIN` before the first device write;
7. write the rescue payload into the reserved `__mbr` program area;
8. flush and compare every payload byte;
9. modify only `osdStart`, `osdSize` and the APA checksum in the **current** live master;
10. write the master last, flush and compare it;
11. run the normal APA parser again.

The saved master inside the Rescue Capsule is identity evidence and historical state. It is not poured wholesale over sector zero. The current disk may contain legitimate metadata changes outside the bootstrap pointer, and recovery should not erase them merely because restoring 1024 bytes is easier to type.

If pointer publication fails after a verified payload write, DriveForge attempts to restore the exact preflight master. The payload may remain in the reserved program area, but an unpublished payload is preferable to a pointer that advertises an unverified one.

## Legacy pointer restore contract

Legacy `HDDMBR*` and `FHDBMBR*` artifacts contain the whole saved master, but restore uses only the saved `osdStart` and `osdSize` after same-disk and live-bounds validation. Payload sectors are not rewritten because those files never contained them. The current master is backed up first and only its pointer fields plus checksum are changed.

This mirrors FHDB Manager. Calling a header backup a full bootstrap backup would be convenient, short and false.

## Recovery architecture

Recovery is not bolted onto `WritableBlockDevice` as a bag of raw-write helpers. The common shape is:

```text
read-only evidence acquisition
        |
        v
immutable source identity + snapshot
        |
        v
pure recovery plan / exact byte diff
        |
        v
mandatory interoperable FHDB artifact set
        |
        v
explicit image-only recovery capability
        |
        v
ordered commit + durable flush + byte readback
        |
        v
cold/normal parser verification
```

Normal HDL/PFS mutation and exceptional APA/bootstrap recovery are different trust domains. They may share byte-level primitives, but they do not share authorization merely because both eventually call `write()`.

## Forensic artifact rules

DriveForge emits the same safety artifacts as FHDB Manager wherever a shared format exists:

- `HDDRAW.BIN` / `HDDRAW2.BIN` - exact original 1024-byte master before exceptional sector-zero repair;
- `HDDMETA.BIN` / `HDDMETA2.BIN` - exact `APAMETA1` v1 image containing every touched original header, per-header SHA-256 and whole-image SHA-256;
- `FORENSIC.TXT` - the shared human-readable map/header report vocabulary, including explicit locked state for incomplete scans;
- `HDDRESCUE.BIN` / `HDDRESCUE2.BIN` - canonical `PS2HBRC\0` Rescue Capsule;
- `HDDMBR.BIN` / `HDDMBR2.BIN` - exact standard master-header backup.

Two-slot binary artifacts follow the FHDB non-overwrite policy. Identical or state-equivalent evidence may be reused. Unrelated existing evidence is never silently replaced because backups becoming a tiny round-robin filesystem would be a fairly hostile feature.

## APA forensic write gate

The host-side recovery executor preserves the same core invariants as FHDB Manager:

1. a truncated scan is read-only;
2. blocking conflict/overlap state cannot authorize writes;
3. topology repair materializes only planner-produced `prev`, `next` and checksum changes;
4. every source header is reread immediately before its write and must still match scan evidence exactly;
5. `HDDMETA` and `FORENSIC.TXT` exist before the first topology write;
6. interior headers are committed before LBA 0;
7. every header is flushed and read back immediately;
8. the complete touched set is reread after the commit sequence;
9. single-master repair first persists and verifies `HDDRAW`;
10. the normal `PhysicalDrive` class remains read-only.

APA's additive checksum is supporting evidence, not absolution. A checksum-only failure cannot identify which protected word changed, while a checksum-valid noncanonical header can hide cancelling corruption. Automatic repair therefore remains deliberately narrow.

## Main/sub-partition UX contract

The partition UI must not require users to understand APA allocation internals merely to recognize one HDL game. A main HDL partition and every sub-partition whose `main_lba` points to it form one logical game group.

The authoritative relationship is `main_lba` plus APA sub-partition number, not adjacency and not the child ID string. This folds unnamed or oddly named sub headers under the correct game while refusing to invent ownership for damaged orphan headers. Corruption is not a matchmaking problem.

For size display, the main `apa::Partition::total_sectors` already includes lengths declared in `subs[]`. The grouped logical size therefore uses the main total directly. Child extents are also summed independently as a consistency signal but are never added again. Otherwise a game could gain disk space by being drawn twice, which would be a groundbreaking storage technology and an unfortunate bug.

The same group identity is the deletion unit. Removing an HDL game targets its main LBA, and the APA removal planner resolves and unlinks the main plus every owned sub header as one operation.

## Physical-write parity gate

DriveForge currently performs recovery writes only through explicit writable image capabilities. FHDB Manager can operate directly on the console HDD because it is already at the hardware endpoint. Host physical writes remain locked until the PC side proves it can preserve the same guarantees despite Windows, USB/SATA bridges, caches and all the other helpful abstractions inserted between intent and platter.

Physical recovery remains locked until:

1. Rescue Capsule create/inspect/restore and legacy pointer restore remain green under deterministic image tests;
2. wrong-disk, changed-disk, corrupt-capsule and stale-plan cases fail before write;
3. every recovery metadata write has the required persistent FHDB before-image artifact;
4. interruption simulations have a deterministic recovery procedure;
5. forensic ambiguity never falls back to the normal APA reader;
6. first/middle/last and multi-header repair ordering is covered by fault injection;
7. sacrificial real-HDD validation reproduces both successful repair and intentional refusal cases;
8. artifacts produced by each platform are round-tripped through the other platform's parser on real samples.

The target is not merely feature parity. The target is that FHDB Manager and DriveForge agree on the bytes, the evidence, the artifact, the operation order and the reasons to refuse. Then the user can choose the console or PC based on convenience instead of choosing which recovery philosophy they trust today.
