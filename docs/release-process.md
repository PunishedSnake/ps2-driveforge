# PS2 DriveForge release process

This document separates machine-verifiable release gates from the real-Windows/real-HDD gate. A green CI run is required, but it cannot prove UAC, SetupAPI, Dokany driver behavior, Explorer integration, or a particular USB/SATA bridge.

## Release train state

The current release train is **0.5.0 “Emilia”**. Emilia is still read-only. The normal Windows package contains the validated Win32 frontend plus the self-contained WinUI frontend under `WinUI\` while WinUI feature/hardware parity is being completed.

The merged Emilia functionality baseline is commit `210d598442641291f12ecb03323d8a91586b6dfd`. Release-prep cleanup is intentionally developed above that baseline so regressions can be compared against a known green state.

## Gate 1 — source safety and architecture

Before an RC:

- source physical devices are opened `GENERIC_READ` only;
- no public source `BlockDevice::write()` capability exists;
- Dokany uses write protection and rejects create/mutation/overwrite/delete paths;
- parser format logic stays outside GUI, SetupAPI, UAC and Dokany layers;
- WinUI does not gain a second APA/PFS parser;
- automatic storage tuning remains conservative for `unknown` media and is not hard-coded to one validation HDD.

Any deliberate writable-HDD work belongs to a later release train with backup/recovery semantics and disposable-image destructive tests.

## Gate 2 — deterministic tests

The complete normal suite must pass on both Windows/MSVC and Linux/Clang sanitizer CI. The current normal suite contains **14 test executables**.

Required coverage includes:

- APA links/checksums/bounds;
- PFS files/directories and SEGI;
- generated-image cross-layer export and SHA-256;
- corruption rejection;
- DriveSession behavior and counters;
- metadata read cache and read-ahead;
- zero-I/O partition catalog and ManagementModel;
- native HDL parsing/enrichment scheduling;
- storage-profile policy;
- NT Dokany open-disposition policy;
- Darkness discovery/mount-letter policy.

## Gate 3 — Windows build and canonical package

Create the candidate using the full release command from [`../BUILDING.md`](../BUILDING.md):

```powershell
.\build-windows.ps1 -Configuration Release -Clean -WithDokany -DokanyRoot '<Dokany 2.3.1 SDK root>'
.\scripts\verify-windows-package.ps1
```

The canonical package verifier is a release gate, not a cosmetic check. It must prove that the staged package contains the legacy GUI, mount CLI, inspector, benchmark, README/CHANGELOG, exactly 14 packaged regression executables, and a usable WinUI payload.

Record the SHA-256 of the final ZIP.

## Gate 4 — benchmark sanity

Performance changes do not need to beat one particular disk on every number, but the candidate must not invalidate Emilia's architecture:

- one APA scan produces the complete partition list;
- `PartitionCatalog` construction performs zero additional device I/O;
- filtering/sorting/selection remain memory-only;
- HDL metadata enrichment is optional/progressive/cancellable;
- PFS warm metadata workloads can be served from session caches;
- unknown/rotational media are not given an aggressive queue depth merely because a synthetic or single-HDD throughput run looked faster.

The reference real-HDD sweep is [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md). Re-run targeted benchmarks after any change to storage policy, cache semantics, overlapped I/O, request ordering, or HDL enrichment.

## Gate 5 — real Windows + real PS2 HDD

Run [`rc-hardware-checklist.md`](rc-hardware-checklist.md) on the exact candidate artifact. This gate must be done after deterministic CI is green.

It covers normal-user startup/UAC, SetupAPI disk discovery, APA auto-open, Explorer mount, known-file read/hash, write rejection, clean unmount, rescan behavior, UAC-cancel/image mode, and occupied-drive-letter fallback.

The physical disk number is not part of the release contract and must not be hard-coded in the report; Windows may renumber `PhysicalDriveN` after reboot/reconnection.

## RC decision

An **0.5.0-rc1 candidate** may be produced when Gates 1–4 are green and the release-prep diff is reviewed. It becomes a validated RC only after Gate 5 passes on the exact artifact.

If Gate 5 fails:

1. preserve the exact artifact SHA and DriveForge commit;
2. preserve the observed Windows/Dokany/Explorer error and relevant logs;
3. reproduce at the narrowest deterministic layer possible;
4. add a regression test when the failure can be represented without hardware;
5. fix on the release branch and restart the affected gates.

Do not rename a failed candidate and ship it unchanged.

## Final 0.5.0 decision

Final **0.5.0** requires:

- all CI green on the release commit;
- canonical Windows artifact verified;
- hardware checklist passed on that artifact or a byte-identical rebuild;
- changelog moved from development wording to released wording/date;
- README limitations accurately reflect what ships;
- no unresolved release-blocking issue in APA/PFS correctness, raw-disk read-only safety, mount lifecycle, or package startup.

WinUI does not have to become the default executable merely to ship 0.5.0. Until feature and hardware parity are demonstrated, the package may continue to carry it as the modern frontend preview beside the validated Win32 fallback.

## Release evidence to preserve

For every RC/final release, retain:

```text
version/codename
commit SHA
CI run links/status
Windows artifact SHA-256
Windows version
Dokany version
source device model/capacity/bus when relevant
APA header/main/sub counts
hardware checklist result
known-file hash result when available
benchmark report if storage policy changed
known limitations
```

This makes later performance/correctness claims auditable and keeps one user's hardware from silently becoming a universal assumption.
