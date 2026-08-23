# Bootstrap provider and automated installation pipeline

Frieren now has enough storage machinery that a future DriveForge release can automate installation and recovery of PS2 HDD bootstrap environments such as FHDB, HDD-OSD, HOSD and PSBBN. The important part is not making a download button. Browsers solved downloading files some time ago. The important part is ensuring that network discovery, cryptographic transformation and raw-disk mutation remain separate trust domains.

This document freezes that architecture before provider-specific code appears. Future maintainers should change these boundaries only with an explicit safety reason, not because putting an HTTP request next to `WritablePhysicalDrive::write()` saved twelve lines.

## Non-negotiable rule

**Bytes obtained from the network never flow directly into a writable PS2 device.**

The pipeline is deliberately staged:

```text
Provider manifest
    |
    v
HTTPS fetch to host cache
    |
    v
content hash + immutable provenance
    |
    v
archive/package extraction to host staging
    |
    v
format-specific inspection
    |
    +-- ELF / KELF structure
    +-- HDD-OSD metadata
    +-- expected files / version evidence
    +-- future MagicGate verification/signing
    |
    v
immutable BootstrapInstallPlan
    |
    v
read-only target preflight + Rescue Capsule/HDDMBR
    |
    v
explicit WritableBlockDevice capability
    |
    v
payload write -> flush -> byte verification
    |
    v
metadata / osdStart-osdSize publication last
    |
    v
release write capability
    |
    v
cold read-only reopen and parser verification
```

Every arrow is an intentional boundary. A provider discovers software. It does not know how APA sectors are written. A bootstrap installer knows the exact target bytes and ordering. It does not perform HTTP requests. A physical backend knows how to obtain a safe Windows raw-disk lease. It does not decide whether an arbitrary KELF is an FHDB build.

## Provider manifest

Providers should be data-driven wherever practical. One manifest entry should describe one installable upstream artifact with fields conceptually equivalent to:

```text
provider id
product family                 FHDB / HDD-OSD / HOSD / PSBBN / other
upstream repository
release or tag selector
asset selector
expected archive/container type
expected SHA-256
expected files after extraction
format-specific inspection policy
license/provenance metadata
optional compatibility constraints
```

The exact serialized schema is not frozen yet. The invariants are.

### Immutable source selection

Normal installation must resolve to an immutable upstream version and exact content hash before the target HDD is opened writable. A mutable `latest` endpoint may be used to discover a candidate release, but the resolved release/tag, asset identity and SHA-256 become part of the frozen install input.

This serves three purposes:

1. a retry installs the same bytes rather than whatever upstream uploaded five minutes later;
2. a recovery report can state exactly what DriveForge attempted to install;
3. tests can use a pinned fixture without depending on the current state of GitHub.

A provider refresh operation may update the local manifest/cache explicitly. A physical mutation must not silently refresh its source halfway through a plan.

## Host cache versus target storage

Downloaded artifacts belong in a host-side cache. Extracted and transformed artifacts belong in a host-side staging directory. Neither location is a PFS partition merely because DriveForge eventually knows how to write PFS.

The target HDD is not a download cache.

Before any physical write, DriveForge should be able to disconnect the network entirely and complete the operation from the frozen staged inputs. This is both testable and useful: loss of internet during an install should be an inconvenience before mutation, not a new failure mode after sector writes began.

## Inspection and transformation

Each product family may require different transformations, but those transformations must produce explicit outputs before storage planning.

Examples include:

- selecting a bootstrap KELF from an archive;
- validating an existing KELF structure;
- generating or preserving HDD-OSD metadata;
- preparing product-specific PFS files;
- determining a requested `osdStart` / `osdSize` payload geometry;
- future software MagicGate verification or signing.

Transformation code must never infer success from a filename alone. `MBR.KELF` is a useful hint supplied by a human or upstream package author, not a cryptographic theorem.

## MagicGate boundary

Software MagicGate support is planned as a separate service/provider boundary. It must not be implemented from remembered constants or a vaguely similar KELF project.

Before signing is enabled, the implementation needs:

1. a canonical upstream algorithm/reference;
2. exact key/material provenance appropriate for the operation;
3. byte-for-byte golden vectors;
4. negative vectors for altered headers, payloads and signatures;
5. separation between `inspect`, `verify` and `sign` operations;
6. deterministic outputs where the real format permits determinism;
7. documented differences, if any, between what a physical PS2/Mechacon path produces and what the host implementation produces.

The bootstrap installer consumes an already inspected/signed result. It does not know how MagicGate works internally. This prevents future crypto work from becoming entangled with APA/PFS recovery semantics.

## Bootstrap install plan

`BootstrapInstallPlan` should be a pure, immutable description created before the write gate opens. At minimum it must freeze:

- target device identity/fingerprint;
- source artifact provenance and SHA-256;
- exact bootstrap payload bytes and their hash;
- target sector start/count inside the reserved `__mbr` area;
- all APA/PFS metadata changes required by the selected product;
- all files to create/replace and their expected final hashes;
- required safety artifacts;
- expected cold-verification evidence.

Planning performs zero target writes.

A stale plan is not repaired in place. If target evidence changes after planning, DriveForge closes/refuses the write lease and makes a new plan from a new read-only snapshot.

## Installation ordering

For MBR/bootstrap payloads, the existing FHDB recovery rule becomes the installation rule too:

1. validate target ownership and device identity;
2. capture a canonical FHDB Rescue Capsule of the current bootstrap when possible;
3. persist a fresh `HDDMBR` before-image;
4. acquire guarded `WritablePhysicalDrive` and Windows volume locks;
5. verify the frozen physical authorization again;
6. write bootstrap payload into reserved `__mbr` storage;
7. flush and read every written payload byte back;
8. write product-specific PFS/APA data that does not yet expose the new bootstrap, when needed;
9. publish the smallest visibility/entry-point metadata last, including `osdStart` / `osdSize` where applicable;
10. flush and parser-verify;
11. release every writable handle and volume lock;
12. reopen through ordinary read-only `PhysicalDrive` and verify the installed product cold.

If product-specific semantics require a different order, the provider/install strategy must document why. It may not quietly move pointer publication earlier because doing so makes progress reporting easier.

## FHDB Rescue Capsule before replacement

A normal replacement install should attempt a full read-only `PS2HBRC` v1 capture of the currently active bootstrap before mutation. That gives DriveForge and FHDB Manager one interoperable artifact containing the exact master plus current payload.

A valid header-only capsule is still meaningful when no bootstrap is published. A corrupt or half-valid bootstrap pointer is not silently normalized during capture. Such a target moves into recovery/diagnostic workflow instead of pretending installation is an appropriate repair tool.

The Rescue Capsule and `PS2DFRC1` Mutation Journal remain different objects:

- Rescue Capsule is historical/bootstrap recovery evidence shared with FHDB Manager;
- Mutation Journal is a DriveForge transaction record for exact before/after metadata ranges.

A future installer may need both.

## Product-specific strategies

FHDB, HDD-OSD, HOSD and PSBBN should become separate strategy/provider implementations over the shared pipeline, not separate raw-disk installers.

A strategy may decide:

- which upstream artifact(s) are required;
- which files are staged;
- whether a MagicGate transformation is required;
- which PFS partitions/directories are required;
- which HDD-OSD metadata is created;
- how product-specific final verification identifies a successful installation.

A strategy may not decide:

- whether `PhysicalDriveN` still refers to the preflight disk;
- whether GPT ownership may be ignored;
- whether Windows volume locks are optional after the plan requested them;
- whether before-image artifacts can be skipped;
- whether a failed readback counts as success.

Those are shared storage safety policies.

## GitHub acquisition

The first provider implementation will use the same injectable HTTP abstraction already used by the OPL asset pipeline, extended as necessary for GitHub release metadata and binary assets. Tests should inject a fake transport. Unit/regression tests must not require GitHub availability.

When GitHub is used as an upstream:

- resolve repository/release metadata before mutation;
- record owner/repository/tag/asset identity;
- bound every download size;
- reject HTML/error pages masquerading as successful binary downloads;
- hash the complete artifact before extraction;
- never execute downloaded host binaries as part of installation;
- prefer parsing/extracting known data formats in-process;
- keep cached artifact provenance next to its hash.

This is supply-chain hygiene, not an accusation that every GitHub release is secretly a tiny villain. Raw-disk software simply has no reason to be casual about provenance.

## Developer-facing comments

Code in this subsystem should comment invariants and reasons, not narrate obvious syntax.

Useful comment:

```cpp
// Publish osdStart/osdSize only after payload readback succeeds. A verified but
// unreachable payload is recoverable; a pointer to partial bytes is boot-visible
// corruption.
```

Useless comment:

```cpp
// Increment i.
++i;
```

When adapting behavior from PS2SDK, FHDB Manager or another canonical implementation, comments should name the upstream concept/function/structure and explain any host-side translation. Future maintainers should be able to answer "why is this byte written last?" without reconstructing a 2026 research session from commit archaeology.

## Current Frieren foundation

The following shared pieces already exist and are intended to be reused by the automated installer:

- read-only `PhysicalDrive` and separate `WritablePhysicalDrive` capability;
- physical media fingerprint and normal/exceptional write authorization;
- GPT/PC ownership guard;
- Windows target-volume lock/dismount lease;
- native APA/HDL/PFS/TAR writers;
- `WriteTransaction` and `PS2DFRC1` Mutation Journal;
- canonical `PS2HBRC` v1 parse/build/validation;
- read-only live Rescue Capsule capture from `osdStart` / `osdSize`;
- bootstrap restore planning and physical bootstrap restore coordinator;
- FHDB-compatible `HDDMBR`, `HDDRAW`, `HDDMETA` and forensic artifacts.

The provider/download layer comes after these pieces are hardware-validated. That order is intentional: downloading the correct FHDB archive is useful, but being able to prove which disk receives it is more fundamental.
