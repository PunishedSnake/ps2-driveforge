# fhdb-bootstrap-manager capability parity plan

PS2 DriveForge and `fhdb-bootstrap-manager` run on different sides of the cable, so parity means **recovery/storage capability parity**, not copying the PS2-side GS renderer or menu implementation. DriveForge should be able to inspect, back up, explain, plan and, once the physical-write gate is earned, recover every HDD state that the PS2-side manager can handle.

This document is a contract for the post-Frieren recovery work. It also prevents recovery features from leaking casually into normal filesystem/game mutation code.

## Status vocabulary

- **Frieren**: present on the current 0.6 development branch.
- **0.7 prototype**: already exists on the parked recovery/forensics branch and must be rebased/ported deliberately, not merged wholesale into Frieren.
- **Planned**: capability still needs a DriveForge implementation.
- **PS2-only**: useful on-console behavior with no meaningful PC-side equivalent.

## Capability matrix

| fhdb-bootstrap-manager capability | DriveForge status | DriveForge module / intended owner |
| --- | --- | --- |
| Validate APA `__mbr`, magic, checksum, bounds | Frieren | `apa::Reader` |
| Enumerate complete APA chain and main/sub relationships | Frieren | `apa::Reader`, `PartitionCatalog` |
| Refuse malformed/non-canonical chains before mutation | Frieren | APA allocation/removal planners |
| Detect protective/hybrid GPT before write | Planned | new `disk_identity` / admission module |
| Inspect HDD OSD bootstrap pointer (`osdStart`, `osdSize`) | Planned | new `bootstrap::MasterInfo` parser |
| Disable only HDD bootstrap pointer | Planned physical-write feature | bootstrap mutation planner, never generic APA editor |
| Header-only APA master backup | Planned | recovery artifact writer |
| Full Rescue Capsule v1 parse/validation | 0.7 prototype | parked `rescue_capsule` module |
| Rescue Capsule SHA-256 verification | 0.7 prototype | parked `rescue_capsule` module |
| Create full Rescue Capsule from current disk | Planned | `recovery::CapsuleWriter` |
| Restore capsule payload before exposing pointer | Planned | staged bootstrap restore transaction |
| Verify source identity immediately before restore | Planned | `disk_identity` + mutation admission |
| Legacy `HDDMBR*.BIN` / `FHDBMBR*.BIN` compatibility | Planned | bootstrap backup import |
| Structural KELF inspection | Planned | `kelf` read-only parser |
| MagicGate signing of stock MBR KELF | Planned | separate signing provider/service boundary |
| Install replacement MBR program inside reserved `__mbr` area | Planned | bootstrap installer with reserved-range proof |
| SHA-256 fingerprints of payload/images | Planned | common digest service |
| Boot-chain evidence report | Planned | `bootstrap::EvidenceReport` |
| FMCB HDD-skip / downstream boot evidence inspection | Planned | host evidence scanners |
| Deterministic structure-health policy | Partial | current APA/PFS diagnostics; needs unified health verdict |
| Exact raw sectors 0-1 snapshot before exceptional repair | Planned | `HDDRAW`-compatible artifact writer |
| Raw forensic APA discovery independent of normal admission | Planned | `forensics::ApaScanner` |
| Forward-link candidate map | Planned | `forensics::ApaGraph` |
| Reverse-link candidate map | Planned | `forensics::ApaGraph` |
| Geometry/reference-derived candidate map | Planned | `forensics::ApaGraph` |
| Conflict/overlap/dormant-free evidence classification | Planned | forensic graph evidence model |
| Read-only shadow APA map browsing | Planned | alternate immutable scan snapshot, never writable session state |
| Truncated-scan fail-closed policy | Planned | forensic admission policy |
| Single-master narrowly reconstructed repair | Planned | exceptional recovery planner |
| Multi-header `prev`/`next` topology repair plan | Planned | forensic repair planner |
| Save every touched original header before repair (`HDDMETA`) | Planned | recovery artifact set |
| Commit non-master headers first, master LBA 0 last | Planned | recovery-specific transaction ordering |
| Flush/read-back/final parser verification | Frieren primitive exists | `WriteTransaction`; recovery adds persistent artifacts/journal |
| Reject stale plan when source changed | Frieren primitive exists | APA mutation/removal stale-link checks; generalize with disk identity |
| Hardware fault injection against images | Planned | host fault-injection tool/tests |
| Hardware fault injection against sacrificial physical HDD | Planned and explicitly gated | later validation tooling |
| Contextual stage/domain errors | Partial | current structured result strings; needs typed error domain |
| Live operation/LBA telemetry | Partial | existing session I/O statistics; mutation telemetry API still needed |
| GS video modes/themes/fonts | PS2-only | no parity requirement beyond equivalent usable Windows UI |

## Recovery architecture

Recovery will not be bolted onto `WritableBlockDevice` as a collection of convenient raw-write helpers. The intended stack is:

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
mandatory recovery artifact set
        |
        v
explicit recovery write capability
        |
        v
ordered commit + durable flush + byte readback
        |
        v
cold reopen / normal parser verification
```

A normal HDL/PFS mutation and an exceptional APA/bootstrap recovery are different trust domains. They may share low-level byte transaction code, but they must not share authorization merely because both eventually call `write()`.

## Artifacts

DriveForge should understand and, where useful, emit artifacts interoperable with the PS2-side manager:

- Rescue Capsule v1;
- exact 1024-byte APA master backups;
- raw sectors 0-1 snapshots comparable to `HDDRAW*.BIN`;
- per-header topology backups comparable to `HDDMETA*.BIN`;
- machine-readable forensic graph/report data plus a human-readable report;
- SHA-256 digests for every recovery-critical payload and original byte range.

The artifact format must identify the source disk strongly enough to prevent a perfectly valid recovery file from being applied to the wrong drive. Capacity alone is not an identity.

## Porting rule for the parked 0.7 branch

The existing Rescue Capsule parser/tests on the parked recovery branch are useful work, but Frieren must not absorb the whole branch by merge. When 0.7 recovery begins, port individual modules onto the then-current `main`, run the current APA/PFS/HDL suite, and keep recovery code behind its own capability/API boundary.

## Gate before physical recovery writes

Physical recovery remains locked until all of the following are true:

1. image-level rescue create/inspect/restore passes cold-reopen tests;
2. wrong-disk and changed-disk identity tests fail closed;
3. every metadata write has a persistent before-image artifact;
4. interrupted commit simulations have a deterministic recovery procedure;
5. forensic ambiguity never silently falls back to the normal APA reader;
6. first/middle/last and multi-header repair ordering is covered by fault injection;
7. sacrificial real-HDD validation reproduces both successful repair and intentional refusal cases.

The point of parity is not to make DriveForge equally capable of damaging a disk. It is to make it equally capable of proving what it intends to do before the first irreversible byte moves.
