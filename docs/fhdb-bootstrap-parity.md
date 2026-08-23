# fhdb-bootstrap-manager capability parity and interchange contract

PS2 DriveForge and `fhdb-bootstrap-manager` run on different sides of the cable, so parity means **recovery/storage capability parity and artifact interoperability**, not copying the PS2-side GS renderer or menu implementation. They are intended to be two sides of the same recovery toolchain: FHDB Bootstrap Manager acquires and repairs on-console, while DriveForge can inspect, validate, preserve and perform the same guarded recovery work from the host side.

The most important rule in this document is deliberately boring because boring storage software is healthy storage software:

> **FHDB Rescue Capsule and DriveForge Mutation Journal are different formats with different purposes and must never be used as synonyms.**

## Stable vocabulary and wire identities

| Term | Wire/file identity | Purpose | Interchange |
| --- | --- | --- | --- |
| **FHDB Rescue Capsule** | `PS2HBRC\0`, v1, normally `HDDRESCUE.BIN` / `HDDRESCUE2.BIN` | Portable snapshot of the exact APA master plus the currently referenced HDD bootstrap payload and integrity metadata | **Shared with `fhdb-bootstrap-manager`** |
| **DriveForge Mutation Journal** | `PS2DFRC1`, v1, normally a host-side `.ps2df-journal` sidecar | Durable before/after record for an in-progress DriveForge metadata transaction and deterministic rollback | DriveForge-private; **not** an FHDB Rescue Capsule |
| APA master backup | exact 1024 bytes, `HDDMBR.BIN` / `HDDMBR2.BIN` | Normal master-header preservation | Shared |
| Exceptional raw master snapshot | exact 1024 bytes, `HDDRAW.BIN` / `HDDRAW2.BIN` | Original sectors 0-1 before exceptional master repair | Shared |
| Forensic touched-header snapshot | `APAMETA1` v1, `HDDMETA.BIN` / `HDDMETA2.BIN` | Original image and SHA-256 of every APA header a forensic topology repair may touch | Shared |
| Human forensic report | `FORENSIC.TXT` | Candidate maps, confidence, discovered headers and write-gate state | Shared schema |

`PS2DFRC1` deliberately keeps its existing development wire magic so an interrupted development transaction does not become unreadable merely because the API was renamed. The semantic/API name is nevertheless **Mutation Journal** everywhere. No `RecoveryCapsule` compatibility alias exists in DriveForge: a file or API called Rescue Capsule is reserved for the FHDB `PS2HBRC\0` format.

Readers fail closed on the wrong format. A Mutation Journal parser must reject `PS2HBRC\0`; an FHDB Rescue Capsule parser must reject `PS2DFRC1`. There is no heuristic cross-format import.

## Status vocabulary

- **Frieren**: present on the current 0.6 development branch.
- **Planned**: capability still needs a DriveForge implementation.
- **Physical-write gate**: host implementation exists or is planned, but raw physical-disk writes remain intentionally unavailable.
- **PS2-only**: useful on-console behavior with no meaningful PC-side equivalent.

## Capability matrix

| fhdb-bootstrap-manager capability | DriveForge status | DriveForge module / intended owner |
| --- | --- | --- |
| Validate APA `__mbr`, magic, checksum, bounds | Frieren | `apa::Reader` |
| Enumerate complete APA chain and main/sub relationships | Frieren | `apa::Reader`, `PartitionCatalog` |
| Group main + authoritative sub-partitions for UX | Frieren | `group_partition_catalog`, `ManagementModel::groups()` |
| Refuse malformed/non-canonical chains before normal mutation | Frieren | APA allocation/removal planners |
| Detect protective/hybrid GPT before write | Frieren | `disk_layout_guard` and exceptional repair blocker |
| Inspect HDD OSD bootstrap pointer (`osdStart`, `osdSize`) | Frieren recovery core | FHDB master/rescue parsing |
| Header-only APA master backup | Frieren | `fhdb_artifacts`, `HDDMBR*.BIN` |
| Full FHDB Rescue Capsule v1 parse/validation | Frieren | `fhdb_rescue` |
| FHDB Rescue Capsule SHA-256 verification | Frieren | `fhdb_rescue`, `sha256` |
| Create full FHDB Rescue Capsule from supplied master/payload | Frieren core | `fhdb_rescue`; disk acquisition/restore orchestration still expanding |
| Preserve exact raw sectors 0-1 before exceptional repair | Frieren | `fhdb_artifacts`, `HDDRAW*.BIN` |
| Raw forensic APA discovery independent of normal admission | Frieren | `apa_forensic` |
| Forward/reverse/geometry candidate maps | Frieren | `apa_forensic` |
| Conflict/overlap/dormant-free evidence classification | Frieren | `apa_forensic` |
| Truncated-scan fail-closed policy | Frieren | `apa_forensic` and recovery executor |
| Single-master narrowly reconstructed repair | Frieren image-only | `apa_repair`, `apa_recovery_apply` |
| Multi-header `prev`/`next` topology repair plan | Frieren image-only | `apa_forensic`, `apa_recovery_apply` |
| Save every touched original header before repair (`HDDMETA`) | Frieren | `fhdb_artifacts`, exact `APAMETA1` v1 |
| Export `FORENSIC.TXT` | Frieren | `fhdb_artifacts` |
| Commit non-master headers first, master LBA 0 last | Frieren image-only | `apa_recovery_apply` |
| Flush/read-back/final touched-set verification | Frieren | `WriteTransaction`, `apa_recovery_apply` |
| Reject stale plan when source changed | Frieren | source-stability reread before each recovery write |
| DriveForge durable mutation rollback journal | Frieren | `mutation_journal`, **not an FHDB artifact** |
| Restore capsule payload before exposing pointer | Planned | FHDB rescue restore coordinator |
| Verify source identity immediately before capsule restore | Planned | FHDB rescue restore coordinator |
| Legacy `HDDMBR*.BIN` / `FHDBMBR*.BIN` restore compatibility | Planned | bootstrap backup import |
| Structural KELF inspection | Frieren Rescue Capsule validation | `fhdb_rescue` compatible structural checks |
| MagicGate signing of stock MBR KELF | Planned | separate signing provider/service boundary |
| Install replacement MBR program inside reserved `__mbr` area | Planned | bootstrap installer with reserved-range proof |
| Boot-chain evidence report | Planned | `bootstrap::EvidenceReport` |
| FMCB HDD-skip / downstream boot evidence inspection | Planned | host evidence scanners |
| Deterministic structure-health policy | Partial | current APA/PFS/forensic diagnostics; needs one unified verdict |
| Read-only shadow APA map browsing | Planned UI | immutable forensic map, never writable normal session state |
| Hardware fault injection against images | Planned | host fault-injection tool/tests |
| Hardware fault injection against sacrificial physical HDD | Planned and explicitly gated | later validation tooling |
| Contextual stage/domain errors | Partial | structured results exist; typed UI domain still expanding |
| Live operation/LBA telemetry | Partial | existing session I/O statistics; mutation telemetry API still needed |
| GS video modes/themes/fonts | PS2-only | no parity requirement beyond equivalent usable Windows UI |

## FHDB Rescue Capsule v1 contract

DriveForge mirrors the FHDB Bootstrap Manager wire format rather than defining a look-alike container. A complete capsule consists of:

```text
0x0000  256 bytes   PS2HBRC v1 metadata
0x0100 1024 bytes   exact APA __mbr header
0x0500 variable     exact sector-aligned active bootstrap payload, if any
```

The v1 metadata records the exact complete size, flags, `osdStart`, `osdSize`, payload bytes, SHA-256 of the master and payload, ROMVER/family/confidence strings and the unpadded KELF file length when structurally valid. Reserved bytes remain zero. Unknown flags or versions are rejected rather than guessed.

A Rescue Capsule is therefore a **portable bootstrap recovery artifact**. It does not describe arbitrary HDL/PFS metadata transactions and it has no PREPARED/COMMITTED/RESTORED lifecycle.

## DriveForge Mutation Journal contract

The Mutation Journal protects one small DriveForge metadata transaction. `PS2DFRC1` records the target device size and non-overlapping sector-aligned ranges with exact before and after bytes. Its lifecycle is:

```text
PREPARED -> COMMITTED
         
         -> RESTORED   (only when a PREPARED transaction is rolled back)
```

The journal is written durably before the protected mutation. Inspection classifies the target as all-before, all-after, mixed or foreign/corrupt. A PREPARED mixed state may be restored to the exact before-images; a COMMITTED journal is never automatically rolled back. This is transaction machinery, not a substitute for `HDDRAW`, `HDDMETA`, `HDDMBR` or `HDDRESCUE`.

## Recovery architecture

Recovery is not bolted onto `WritableBlockDevice` as a collection of convenient raw-write helpers. The stack is:

```text
read-only evidence acquisition
        |
        v
immutable source identity + forensic snapshot
        |
        v
pure recovery plan / exact byte diff
        |
        v
mandatory interoperable FHDB artifact set
        |
        v
explicit image-only recovery write capability
        |
        v
ordered commit + durable flush + byte readback
        |
        v
cold reopen / normal parser verification
```

A normal HDL/PFS mutation and an exceptional APA/bootstrap recovery are different trust domains. They may share low-level byte transaction primitives, but they do not share authorization merely because both eventually call `write()`.

## Forensic artifact rules

DriveForge emits the same safety artifacts as the PS2-side manager where the format already exists:

- `HDDRAW.BIN` / `HDDRAW2.BIN`: exact original 1024-byte master before exceptional sector-zero repair;
- `HDDMETA.BIN` / `HDDMETA2.BIN`: exact `APAMETA1` v1 image containing every touched original header, per-header SHA-256 and whole-image SHA-256;
- `FORENSIC.TXT`: the same human-readable map/header report vocabulary, including explicit `LOCKED` state for incomplete scans;
- `HDDRESCUE.BIN` / `HDDRESCUE2.BIN`: canonical `PS2HBRC\0` Rescue Capsule;
- `HDDMBR.BIN` / `HDDMBR2.BIN`: exact standard master-header backup.

Two-slot binary artifacts follow the FHDB non-overwrite policy: an already identical artifact may be reused, but unrelated existing evidence is never silently replaced.

## APA forensic write gate

The host-side recovery executor preserves the same core invariants as `fhdb-bootstrap-manager`:

1. a truncated scan is read-only, full stop;
2. candidate maps with blocking conflict/overlap state cannot authorize writes;
3. only planner-produced `prev`, `next` and checksum changes are materialized for topology repair;
4. every source header is reread immediately before its write and must still match the scan evidence byte-for-byte;
5. `HDDMETA` and `FORENSIC.TXT` exist before the first topology write;
6. interior headers are committed before LBA 0;
7. each header is flushed and read back immediately;
8. the complete touched set is reread after the commit sequence;
9. a single-master repair first persists and verifies `HDDRAW`;
10. the normal `PhysicalDrive` class remains read-only.

APA's additive checksum is supporting evidence, not proof. A checksum-only failure cannot identify which protected word changed, and a checksum-valid but noncanonical header can conceal cancelling corruption. The single-master planner therefore accepts only narrowly reconstructable fields whose exact canonical correction is independently identified and corroborated by the stale checksum.

## Main/sub-partition UX contract

The partition UI must not force users to understand APA allocation internals merely to recognize one HDL game. A main HDL partition and every sub-partition whose `main_lba` points to it form one logical game group.

The authoritative relationship is `main_lba` plus APA sub-partition number, **not adjacency and not the child ID string**. This naturally folds unnamed or oddly named sub headers under the correct game while refusing to invent ownership for damaged orphan headers.

For size display, the main `apa::Partition::total_sectors` already includes lengths declared in `subs[]`. The grouped logical size therefore uses the main size directly. Child extents are also summed independently as a consistency signal, but are never added to the logical size a second time. Otherwise a two-part game would mysteriously gain storage by being rendered, which would be an impressive but unhelpful filesystem feature.

The same group identity is the natural deletion unit: selecting/removing an HDL game targets its main LBA, and the existing APA removal planner already resolves and unlinks the main plus every owned sub header as one operation.

## Gate before physical recovery writes

Physical recovery remains locked until all of the following are true:

1. image-level Rescue Capsule create/inspect/restore passes cold-reopen tests;
2. wrong-disk and changed-disk identity tests fail closed;
3. every metadata write has the required persistent FHDB before-image artifact;
4. interrupted commit simulations have a deterministic recovery procedure;
5. forensic ambiguity never silently falls back to the normal APA reader;
6. first/middle/last and multi-header repair ordering is covered by fault injection;
7. sacrificial real-HDD validation reproduces both successful repair and intentional refusal cases.

The point of parity is not to make DriveForge equally capable of damaging a disk. It is to make both tools agree on the evidence, the artifact, the plan and the refusal before the first irreversible byte moves.
