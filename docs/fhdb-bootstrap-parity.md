# FHDB Manager parity and interchange contract

PS2 DriveForge and **FHDB Manager** are two frontends to the same PS2 HDD recovery problem. FHDB Manager runs on the console. DriveForge runs on the host PC. The repository for FHDB Manager is still named `fhdb-bootstrap-manager` for historical reasons, because repository names apparently enjoy a longer half-life than product names.

Parity means **recovery capability parity, safety-policy parity and artifact interoperability**. It does not mean reproducing the PS2 UI on Windows. A backup created on one platform should be understandable on the other whenever the format has a useful cross-platform meaning, and both tools should reach the same refusal decision from the same evidence.

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

`PS2DFRC1` keeps its original development wire magic so an interrupted DriveForge transaction does not become unreadable merely because the API was renamed. The semantic/API name is **Mutation Journal** everywhere. If a DriveForge API says Rescue Capsule, it means the FHDB `PS2HBRC\0` format.

Readers fail closed on the wrong format. A Mutation Journal parser rejects `PS2HBRC\0`; an FHDB Rescue Capsule parser rejects `PS2DFRC1`. There is no heuristic cross-format import. Eight magic bytes are cheap. Recovering from a tool that guessed wrong is less cheap.

## Capability matrix

| FHDB Manager capability | DriveForge status | DriveForge owner |
| --- | --- | --- |
| Validate APA `__mbr`, magic, checksum and bounds | Frieren | `apa::Reader` |
| Enumerate complete APA chain and main/sub relationships | Frieren | `apa::Reader`, `PartitionCatalog` |
| Refuse malformed/noncanonical chains before normal mutation | Frieren | APA mutation planners |
| Detect protective/hybrid GPT evidence before write | Frieren | `disk_layout_guard`, physical write admission |
| Keep ordinary raw-disk reader structurally read-only | Frieren | `PhysicalDrive` |
| Explicit raw-disk write capability | Frieren / Windows | `WritablePhysicalDrive` |
| Freeze/recheck physical media identity around RW lease | Frieren / Windows | `physical_write_guard` |
| Lock/dismount Windows volumes belonging to target disk | Frieren / Windows | `WritablePhysicalDrive` |
| Inspect HDD OSD bootstrap pointer (`osdStart`, `osdSize`) | Frieren | FHDB master/rescue parsing |
| Header-only APA master backup | Frieren | `fhdb_artifacts`, `HDDMBR*.BIN` |
| Full Rescue Capsule v1 parse/validation | Frieren | `fhdb_rescue` |
| Rescue Capsule SHA-256 verification | Frieren | `fhdb_rescue`, `sha256` |
| Create canonical Rescue Capsule from supplied master/payload | Frieren | `fhdb_rescue` |
| Capture live Rescue Capsule directly from a disk read-only | Frieren | `fhdb_rescue_capture` |
| Discover `HDDRESCUE*` slots with FHDB Manager precedence | Frieren | `fhdb_restore` |
| Restore capsule payload before exposing pointer | Frieren | `fhdb_restore` |
| Verify same-disk identity before and immediately before restore | Frieren | `fhdb_restore`, physical write guard |
| Validate payload against live `__mbr` geometry | Frieren | `fhdb_restore`, `fhdb_rescue_capture` |
| Save current `HDDMBR*` before bootstrap restore | Frieren | `fhdb_restore`, `fhdb_artifacts` |
| Legacy `HDDMBR*` / `FHDBMBR*` pointer-only restore | Frieren | `fhdb_restore` |
| Invalid/wrong-disk full rescue blocks legacy fallback | Frieren | `fhdb_restore` |
| Physical bootstrap/capsule restore with cold reopen verification | Frieren / Windows | `physical_bootstrap_recovery` |
| Structural KELF inspection for Rescue Capsule | Frieren | `fhdb_rescue` |
| Preserve exact raw sectors 0-1 before exceptional repair | Frieren | `fhdb_artifacts`, `HDDRAW*.BIN` |
| Raw forensic APA discovery independent of normal admission | Frieren | `apa_forensic` |
| Forward/reverse/geometry candidate maps | Frieren | `apa_forensic` |
| Conflict/overlap/dormant-free evidence classification | Frieren | `apa_forensic` |
| Truncated-scan fail-closed policy | Frieren | `apa_forensic`, recovery executor |
| Single-master narrowly reconstructed repair | Frieren / image | `apa_repair`, `apa_recovery_apply` |
| Multi-header `prev`/`next` topology repair plan | Frieren / image | `apa_forensic`, `apa_recovery_apply` |
| Exceptional physical authorization without demanding healthy APA | Frieren / Windows | `physical_write_guard` |
| Save every touched original header before repair | Frieren | `fhdb_artifacts`, `HDDMETA*.BIN` |
| Export `FORENSIC.TXT` | Frieren | `fhdb_artifacts` |
| Commit non-master headers first, master LBA 0 last | Frieren / image | `apa_recovery_apply` |
| Flush/read-back/final touched-set verification | Frieren | recovery executors |
| Reject stale plans when source changed | Frieren | frozen source snapshots + immediate reread |
| DriveForge durable metadata rollback journal | Frieren | `mutation_journal`, **not an FHDB artifact** |
| Physical HDL install/remove using the same core writers as images | Frieren / Windows | physical deployment/removal coordinators |
| MagicGate signing/verification of installable KELF | Planned | separate signing provider/service boundary |
| Automated FHDB/HDD-OSD/HOSD/PSBBN acquisition | Planned | provider pipeline, host cache only |
| Automated bootstrap installation from frozen provider inputs | Planned | bootstrap installer |
| Physical forensic APA repair | In progress | exceptional recovery coordinator over existing planners |
| Boot-chain evidence report | Planned parity | bootstrap evidence service |
| FMCB HDD-skip / downstream boot evidence inspection | Planned parity | host evidence scanners |
| Deterministic unified structure-health verdict | Partial | APA/PFS/forensic diagnostics |
| Read-only shadow forensic APA map browsing | Planned UI | immutable forensic snapshot |
| Hardware fault injection against images | Planned | recovery fault-injection tests |
| Hardware fault injection against sacrificial physical HDD | Required before normal GUI exposure | hardware validation tooling |
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

### Live capture

`capture_rescue_image()` converts current disk state into this wire format without writing the disk. It deliberately follows only the published APA master fields:

1. inspect PC MBR/GPT ownership and refuse conflicting ownership;
2. read the exact 1024-byte master;
3. require a canonical `__mbr` master;
4. read `osdStart` and `osdSize` from the live header;
5. accept zero/zero as a valid header-only snapshot;
6. reject a half-empty pointer;
7. require payload start at or after sector `0x2000`;
8. limit payload capture to the FHDB 4 MiB policy;
9. require the complete range to fit the live `__mbr` partition and backing device;
10. read those exact sectors and build the normal Rescue Capsule.

Capture does **not** scan the disk for a more plausible KELF. The point of a backup is to preserve what is published, not to rewrite history while collecting evidence. Structural KELF validation controls only whether `VALID_KELF` is set.

## DriveForge Mutation Journal contract

The Mutation Journal protects one small DriveForge metadata transaction. `PS2DFRC1` records target device size and non-overlapping sector-aligned ranges with exact before and after bytes. Its lifecycle is:

```text
PREPARED -> COMMITTED
         -> RESTORED   only when a PREPARED transaction is rolled back
```

The journal is written durably before the protected mutation. Inspection classifies the target as all-before, all-after, mixed or foreign/corrupt. A PREPARED mixed state may be restored to exact before-images. A COMMITTED journal is never automatically rolled back. This is transaction machinery, not a substitute for `HDDRAW`, `HDDMETA`, `HDDMBR` or `HDDRESCUE`.

## Bootstrap restore discovery contract

DriveForge mirrors FHDB Manager restore discovery. The order is part of interoperability, not an implementation detail:

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

If `HDDRESCUE.BIN` exists but is corrupt or belongs to another disk, quietly using an older `HDDMBR.BIN` would hide the strongest evidence that the requested restore set is inconsistent. Both tools refuse instead.

Header-only Rescue Capsules are different. They represent a valid snapshot with bootstrap disabled, so they permit legacy pointer fallback exactly as FHDB Manager does.

## Bootstrap restore ordering

Image and physical restore intentionally share the same format-layer executor. The Windows physical coordinator only owns endpoint admission, volume locking and cold verification.

The common restore ordering is:

1. validate the Rescue Capsule or legacy pointer source completely;
2. prove same-disk identity against the live master;
3. validate `payload_start` and `payload_sectors` against live `__mbr` geometry and the 4 MiB FHDB limit;
4. freeze the complete current master in the restore plan;
5. immediately before apply, reread the current master and require byte-for-byte equality with the frozen source;
6. save and verify current master as `HDDMBR.BIN` or `HDDMBR2.BIN` before the first device write;
7. write full rescue payload into the reserved `__mbr` program area when present;
8. flush and compare every payload byte;
9. modify only `osdStart`, `osdSize` and APA checksum in the **current** live master;
10. write the master last, flush and compare it;
11. run the normal APA parser again.

The saved master inside the Rescue Capsule is identity evidence and historical state. It is not poured wholesale over sector zero. The current disk may contain legitimate metadata changes outside the bootstrap pointer.

If pointer publication fails after a verified payload write, DriveForge attempts to restore the exact preflight master. The payload may remain in the reserved program area, but an unpublished payload is preferable to a pointer that advertises unverified bytes.

For a physical restore, `WritablePhysicalDrive` is destroyed before final verification. A new ordinary `PhysicalDrive` then rereads the master, APA chain and full payload where applicable. A successful restore therefore does not depend on an RW handle or writer-side state still being alive.

## Legacy pointer restore contract

Legacy `HDDMBR*` and `FHDBMBR*` artifacts contain the whole saved master, but restore uses only saved `osdStart` and `osdSize` after same-disk and live-bounds validation. Payload sectors are not rewritten because those files never contained them. Current master is backed up first and only pointer fields plus checksum change.

Calling a header backup a full bootstrap backup would be convenient, short and false.

## Physical write authorization

Windows physical mutation is a separate capability. The ordinary `PhysicalDrive` remains `GENERIC_READ` and never gains a write method.

Normal mutation uses:

```text
PhysicalDrive read-only
  -> PC/GPT ownership guard
  -> clean canonical APA
  -> five-window SHA-256 media fingerprint
  -> WritablePhysicalDrive RW reopen
  -> verify authorization
  -> lock/dismount every Windows volume mapped to target disk
  -> verify authorization again
  -> mutation
```

`PhysicalDriveN` is an address, not an identity. Windows may renumber disks. The fingerprint freezes device size plus widely separated sampled windows so a changed/replaced source loses authorization before the first write.

Exceptional recovery is deliberately different. A damaged APA cannot satisfy a rule that requires a healthy APA before it may be repaired. `authorize_exceptional_recovery()` therefore freezes PC/GPT ownership and physical identity **without** requiring the normal APA reader to accept the disk. This does not authorize arbitrary writes. The forensic/repair planner still has to prove the exact recovery bytes and required artifacts.

Normal mutation and exceptional recovery are different trust domains even though both ultimately produce a `WritableBlockDevice` lease.

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
explicit writable endpoint capability
        |
        v
ordered commit + durable flush + byte readback
        |
        v
release writable endpoint
        |
        v
cold/normal parser verification
```

Images and physical disks share format logic. Physical media add a stricter endpoint gate; they do not get a second interpretation of APA/FHDB bytes.

## Forensic artifact rules

DriveForge emits the same safety artifacts as FHDB Manager wherever a shared format exists:

- `HDDRAW.BIN` / `HDDRAW2.BIN` - exact original 1024-byte master before exceptional sector-zero repair;
- `HDDMETA.BIN` / `HDDMETA2.BIN` - exact `APAMETA1` v1 image containing every touched original header, per-header SHA-256 and whole-image SHA-256;
- `FORENSIC.TXT` - shared human-readable map/header report vocabulary, including explicit locked state for incomplete scans;
- `HDDRESCUE.BIN` / `HDDRESCUE2.BIN` - canonical `PS2HBRC\0` Rescue Capsule;
- `HDDMBR.BIN` / `HDDMBR2.BIN` - exact standard master-header backup.

Two-slot binary artifacts follow the FHDB non-overwrite policy. Identical or state-equivalent evidence may be reused. Unrelated existing evidence is never silently replaced because backups becoming a tiny round-robin filesystem would be a fairly hostile feature.

## APA forensic write gate

The recovery executor preserves these core invariants:

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

The remaining Frieren work is to wrap these existing image recovery planners/executors in the exceptional physical authorization/lease and cold-reopen pattern. No new heuristic repair algorithm is needed or wanted merely because a real HDD is now writable.

## Main/sub-partition UX contract

The partition UI must not require users to understand APA allocation internals merely to recognize one HDL game. A main HDL partition and every sub-partition whose `main_lba` points to it form one logical game group.

The authoritative relationship is `main_lba` plus APA sub-partition number, not adjacency and not the child ID string. This folds unnamed or oddly named sub headers under the correct game while refusing to invent ownership for damaged orphan headers. Corruption is not a matchmaking problem.

For size display, the main `apa::Partition::total_sectors` already includes lengths declared in `subs[]`. The grouped logical size therefore uses the main total directly. Child extents are also summed independently as a consistency signal but are never added again. Otherwise a game could gain disk space by being drawn twice, which would be a groundbreaking storage technology and an unfortunate bug.

The same group identity is the deletion unit. Removing an HDL game targets its main LBA, and the APA removal planner resolves and unlinks the main plus every owned sub header as one operation.

## Automated bootstrap installation

FHDB/HDD-OSD/HOSD/PSBBN acquisition and installation is a planned layer over this foundation. Its contract is documented in [`bootstrap-provider-pipeline.md`](bootstrap-provider-pipeline.md).

The short version is deliberately boring:

```text
GitHub/upstream provider
 -> bounded host download
 -> pinned release/asset provenance + SHA-256
 -> host-side extraction and inspection
 -> future MagicGate service where required
 -> immutable BootstrapInstallPlan
 -> Rescue Capsule + HDDMBR before-image
 -> guarded writable endpoint
 -> payload first, pointer/visibility last
 -> cold verification
```

Network bytes never flow directly into `WritablePhysicalDrive`. Provider code does not know sector-write APIs. Storage code does not decide which GitHub release is legitimate. MagicGate code will be a separate service backed by canonical algorithm research and golden vectors, not an adventurous collection of remembered constants.

## Hardware gate

Physical writing now exists as an explicit developer capability, but normal user-facing GUI exposure remains gated on real-hardware validation. The distinction matters: implemented code and validated-on-sacrificial-hardware code are not synonyms.

Before physical recovery/install controls become ordinary UI actions, require:

1. successful and intentional-refusal runs on sacrificial PS2 HDD hardware;
2. SATA and at least one common USB bridge path where possible;
3. Rescue Capsule creation on DriveForge parsed by FHDB Manager and vice versa;
4. full bootstrap restore with cold boot validation on a physical PS2;
5. interrupted/failed write drills with deterministic artifact-based recovery;
6. physical HDL install/remove followed by OPL/HDLoader visibility validation;
7. wrong-disk/change-disk simulation proving the fingerprint gate refuses mutation;
8. forensic physical repair after the exceptional recovery coordinator is finished.

The target is not merely feature parity. The target is that FHDB Manager and DriveForge agree on the bytes, evidence, artifacts, operation order and reasons to refuse. Then the user can choose console or PC based on convenience instead of choosing which recovery philosophy they trust today.
