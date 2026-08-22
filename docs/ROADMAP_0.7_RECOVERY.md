# PS2 DriveForge 0.7 — Recovery / Diagnostics / Forensics

This document parks the intended 0.7 direction while 0.5/0.6 work continues elsewhere. It is a design target, not a release-date commitment.

## Release identity

0.7 is intended to be the first large post-Emilia expansion: full frontend feature parity plus a dedicated **Recovery Center** for damaged, suspicious, partially readable, or historically important PS2 HDDs and images.

The initial implementation remains read-only with respect to source physical disks. Recovery work should prefer export, imaging, comparison and reconstructed copies before direct metadata repair is considered.

## Recovery Center scope

- physical-HDD and image diagnostics;
- resumable imaging with bad-sector/retry maps;
- source/image SHA-256 verification;
- APA chain/integrity analysis;
- orphan, overlapping, broken-link and out-of-range partition diagnostics;
- PFS superblock/inode/SEGI/directory diagnostics;
- HDL metadata diagnostics;
- forensic mode for partially damaged structures;
- JSON/text reports and disk maps;
- safe extraction/recovery to the host filesystem;
- Rescue Capsule inspection and comparison;
- recovery-plan generation from current media + known-good capsule state.

## Rescue Capsules

The canonical format is **not defined by DriveForge**. DriveForge follows `fhdb-bootstrap-manager/docs/RESCUE_FORMAT.md`.

Version 1 layout:

```text
0x0000  0x0100  versioned capsule metadata
0x0100  0x0400  exact 1024-byte APA __mbr header
0x0500  variable exact sector-aligned bootstrap payload, when present
```

DriveForge readers must preserve the format's compatibility policy:

- magic `PS2HBRC\0`;
- version 1 only until another version is explicitly implemented;
- unsupported versions are rejected, never guessed;
- exact complete-size relationships are checked;
- unknown flag bits are rejected;
- APA and optional payload SHA-256 values are verified;
- the embedded master header must be a structurally valid `__mbr`;
- same-disk identity ignores only the APA checksum and mutable `osdStart` / `osdSize` fields;
- `VALID_KELF` requires a payload and a non-zero structural KELF length.

The first implementation lives in `include/ps2hdd/rescue_capsule.hpp` and `src/core/rescue_capsule.cpp`, with compatibility vectors in `tests/rescue_capsule_tests.cpp`.

## Planned Capsule UX

A future WinUI Recovery Center should support at least:

1. **Inspect Capsule** without a connected disk.
2. **Compare Capsule with HDD/image**.
3. Field-aware APA master-header diff.
4. Bootstrap pointer/range comparison.
5. Payload digest comparison.
6. ROMVER / probable-family / confidence display.
7. Recovery confidence and an explainable recovery plan.

Example conceptual flow:

```text
known-good Rescue Capsule
          +
current HDD / image
          |
          v
forensic comparison
          |
          +-- disk identity match / mismatch
          +-- APA differences
          +-- bootstrap pointer differences
          +-- payload present / missing / changed
          +-- KELF structural state
          +-- recommended read-only recovery/export actions
```

## Deliberate boundary

0.7's first goal is **inspection, diagnosis, imaging, comparison and recovery to safe destinations**.

The fhdb-bootstrap-manager specification already documents a careful write-capable restoration sequence (payload write -> flush -> sector comparison -> `HDIOC_SETOSDMBR` -> final verification). DriveForge may eventually expose compatible restoration, but direct physical-HDD write support is intentionally not part of the initial Rescue Capsule parser or this parked foundation.

Any future write-capable recovery path must be separately gated, explicitly enabled and validated against disk images and disposable hardware before public release.
