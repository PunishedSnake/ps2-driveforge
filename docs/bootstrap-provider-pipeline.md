# Bootstrap provider and automated installation pipeline

Frieren now has the storage, recovery and MagicGate foundations needed for automated installation of PS2 HDD bootstrap environments such as FHDB, HDD-OSD, HOSD and PSBBN. Provider-specific acquisition/strategy code remains a separate integration workstream; the safety and cryptographic boundaries it must use are implemented and frozen here.

The important part is not making a download button. The important part is ensuring that network discovery, cryptographic transformation and raw-disk mutation remain separate trust domains.

## Non-negotiable rule

**Bytes obtained from the network never flow directly into a writable PS2 device.**

The pipeline is deliberately staged:

```text
Provider manifest
    |
    v
bounded HTTPS fetch to host cache
    |
    v
content SHA-256 + immutable provenance
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
    |
    v
MagicGateHostService
    |
    +-- key-free inspect
    +-- local-keyset verify/sign
    +-- optional explicit MechaCon ICVPS2 evidence
    |
    v
independently verified staged payload
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

Every arrow is an intentional boundary. A provider discovers software. It does not know how APA sectors are written. The MagicGate service can inspect/verify/sign bytes but has no writer. A bootstrap installer knows exact target bytes and ordering but performs no HTTP requests. A physical backend can obtain a guarded Windows raw-disk lease but does not decide whether an arbitrary KELF is an FHDB build.

## Provider manifest

Providers should be data-driven wherever practical. One manifest entry should describe one installable upstream artifact with fields equivalent to:

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
MagicGate policy               none / verify / sign
ICVPS2 policy                  not required / explicit hardware evidence required
license/provenance metadata
optional compatibility constraints
```

The serialized schema may evolve. The invariants do not.

### Immutable source selection

Normal installation resolves to an immutable upstream version and exact content hash before the target HDD is opened writable. A mutable `latest` endpoint may discover a candidate release, but the resolved release/tag, asset identity and SHA-256 become frozen install input.

A provider refresh may update the host cache explicitly. A physical mutation may not silently refresh its source halfway through a plan.

## Host cache versus target storage

Downloaded artifacts belong in a host-side cache. Extracted and transformed artifacts belong in a host-side staging directory. Neither location is a PFS partition merely because DriveForge eventually writes PFS.

Before any physical write, DriveForge must be able to disconnect the network and finish from frozen staged inputs. Loss of internet should be an inconvenience before mutation, not a new failure mode after sector writes begin.

## Inspection and transformation

Each product strategy may require different transformations, but all transformations produce explicit host-side outputs before storage planning.

Examples:

- selecting a bootstrap KELF from an archive;
- validating an existing KELF structure;
- verifying or signing a supported low-layout disk KELF through `MagicGateHostService`;
- supplying explicit MechaCon/reference ICVPS2 evidence when the KELF requests it;
- generating or preserving HDD-OSD metadata;
- preparing product-specific PFS files;
- determining requested `osdStart` / `osdSize` payload geometry.

Transformation code never infers cryptographic success from a filename.

## MagicGate boundary - implemented

Host-side MagicGate is no longer a future placeholder. Frieren implements the supported common low-layout disk KELF path through `MagicGateHostService`.

The service separates:

```text
inspect
verify
sign
local software keyset capability
MechaCon ICVPS2 evidence capability
```

DriveForge ships no Sony key material. Local keysets are injected explicitly, strictly parsed and provenance-labelled.

ICVPS2 is not guessed from the software keyset. PS2SDK obtains the eight-byte value from MechaCon command `0x98`, so Frieren treats it as separate hardware/reference evidence. KELFs requiring ICVPS2 fail closed when that evidence is absent or mismatched.

Generated KELFs are reconsumed through the independent verifier before the service returns success. See [`magicgate-host.md`](magicgate-host.md).

Provider code therefore consumes MagicGate as a capability rather than assembling DES/Kbit/Kc/signature steps itself.

## Bootstrap install plan

`BootstrapInstallPlan` is a pure immutable description created before the write gate opens. It must freeze at minimum:

- target device identity/fingerprint;
- source artifact provenance and SHA-256;
- source product/version evidence;
- exact bootstrap payload bytes and hash;
- MagicGate verification/signing result and non-secret provenance metadata;
- whether ICVPS2 evidence was required and its non-secret provenance;
- target sector start/count inside reserved `__mbr` storage;
- APA/PFS metadata changes required by the strategy;
- files to create/replace and expected final hashes;
- required safety artifacts;
- expected cold-verification evidence.

Planning performs zero target writes.

A stale plan is not repaired in place. If target evidence changes after planning, DriveForge refuses the write lease and builds a new plan from a fresh read-only snapshot.

## Installation ordering

For MBR/bootstrap payloads, the existing FHDB recovery rule is also the installation rule:

1. validate target ownership and device identity;
2. capture a canonical FHDB Rescue Capsule of the active bootstrap when possible;
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

A product strategy that needs a different ordering must document why. It may not move pointer publication earlier because progress reporting looks nicer.

## FHDB Rescue Capsule before replacement

A normal replacement install should attempt a full read-only `PS2HBRC` v1 capture of the active bootstrap before mutation. That gives DriveForge and FHDB Manager one interoperable artifact containing the exact master plus current payload.

A valid header-only capsule remains useful when no bootstrap is published. A corrupt bootstrap pointer moves into recovery/diagnostic workflow rather than being silently normalized by an installer.

The Rescue Capsule and `PS2DFRC1` Mutation Journal remain different objects:

- Rescue Capsule is historical/bootstrap recovery evidence shared with FHDB Manager;
- Mutation Journal is a DriveForge transaction record for exact metadata before/after ranges.

A bootstrap install may need both.

## Product-specific strategies

FHDB, HDD-OSD, HOSD and PSBBN should be separate strategy/provider implementations over the shared pipeline, not separate raw-disk installers.

A strategy may decide:

- which upstream artifacts are required;
- which files are staged;
- whether MagicGate verify/sign is required;
- whether ICVPS2 evidence is required;
- which PFS partitions/directories are required;
- which HDD-OSD metadata is created;
- how final verification identifies a successful installation.

A strategy may not decide:

- whether `PhysicalDriveN` still refers to the preflight disk;
- whether GPT ownership may be ignored;
- whether volume locks are optional after the plan requested them;
- whether before-image artifacts can be skipped;
- whether a failed readback counts as success;
- whether missing MagicGate/ICVPS2 capabilities may be replaced by guessed constants.

Those are shared safety/cryptographic policies.

## GitHub acquisition

Provider implementations use the injectable HTTP abstraction already employed by the OPL asset pipeline, extended as needed for release metadata and binary assets. Tests inject fake transport. Regression tests must not depend on GitHub availability.

When GitHub is upstream:

- resolve repository/release metadata before mutation;
- record owner/repository/tag/asset identity;
- bound every download size;
- reject HTML/error pages masquerading as binary downloads;
- hash the complete artifact before extraction;
- never execute downloaded host binaries as part of installation;
- prefer parsing/extracting known data formats in-process;
- keep cached artifact provenance next to its hash.

## Current Frieren foundation

The shared foundation available to provider strategies now includes:

- read-only `PhysicalDrive` and separate `WritablePhysicalDrive` capability;
- physical media fingerprint and normal/exceptional write authorization;
- GPT/PC ownership guard;
- Windows target-volume lock/dismount lease;
- native APA/HDL/PFS/TAR writers;
- `WriteTransaction` and `PS2DFRC1` Mutation Journal;
- `PS2HBRC` v1 parse/build/validation;
- live read-only Rescue Capsule capture from `osdStart` / `osdSize`;
- bootstrap restore planning and physical bootstrap restore coordinator;
- FHDB-compatible `HDDMBR`, `HDDRAW`, `HDDMETA` and forensic artifacts;
- complete bounded low-layout host MagicGate inspect/verify/sign service;
- explicit MechaCon/reference ICVPS2 evidence capability;
- dedicated cross-platform MagicGate regression CI.

## Remaining provider integration work

The unfinished part is product/provider automation, not the MagicGate algorithm stack. Before automated FHDB/HDD-OSD/HOSD/PSBBN installation can be advertised as release-ready, each supported strategy still needs pinned real upstream fixtures, immutable plan generation and sacrificial-hardware validation.

That order remains intentional: downloading the right archive is useful, but proving what bytes we are about to cryptographically bless and which disk will receive them is more fundamental.
