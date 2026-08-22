# Development and documentation guidelines

DriveForge should remain understandable to a contributor who did not participate in the original reverse-engineering work. Documentation, tests and evidence are part of the implementation rather than cleanup postponed until 1.0.

## Definition of done

For meaningful storage/parser/performance/frontend changes, handle the applicable parts of:

1. implementation in the correct layer;
2. focused deterministic regression coverage;
3. generated-image E2E when several layers interact;
4. corruption coverage when malformed metadata is involved;
5. Windows/MSVC CI;
6. Linux Clang ASan+UBSan+warnings-as-errors CI;
7. canonical package verification for Windows staging changes;
8. real-hardware validation for UAC/SetupAPI/raw storage/Dokany/Explorer/device-specific behavior;
9. README/changelog/design/testing docs when visible behavior or an invariant changed;
10. comments next to non-obvious units, safety rules and format/API traps.

The current normal Emilia suite contains **14 regression executables**. See [`testing.md`](testing.md).

## Comment policy

Comments should explain **why**, units, invariants, evidence and traps. Avoid narrating obvious C++.

Good candidates:

- conversion among bytes, 512-byte sectors, PFS metadata blocks and zones;
- APA main/sub extent translation;
- checksum/bounds checks that look redundant but reject damaged metadata;
- SEGD/SEGI traversal rules;
- PFS directory 512-byte boundary behavior;
- HDLoader allocation-table length units;
- why cache entries are validated/immutable;
- why rotational/unknown scheduling is physical-LBA ordered/conservative;
- Windows raw-disk `GENERIC_READ` and explicit-offset OVERLAPPED rules;
- NT Dokany `FILE_*` create dispositions versus Win32 `CreateFileW` constants.

Bad:

```cpp
// Increment offset.
offset += take;
```

Useful:

```cpp
// BlockInfo.number is a PFS zone number here, not a physical sector.
// ApaVolume applies the selected APA extent's LBA translation.
```

Prefer comments that survive refactors: describe the invariant rather than a temporary variable name.

## README responsibilities

README should answer:

- what DriveForge is;
- current version/codename;
- what works now;
- what is explicitly not implemented;
- source-safety status;
- how to build/run or where to find exact build instructions;
- where deeper architecture/testing/release docs live;
- current WinUI/legacy frontend status without claiming parity that has not been tested.

Do not turn README into a complete APA/PFS specification.

## Architecture responsibilities

[`architecture.md`](architecture.md) owns dependency direction and component boundaries.

Current invariants include:

- `ps2driveforge_core` does not depend on GUI/XAML/SetupAPI/UAC/Dokany;
- format parsing lives in core translation units (`apa`, `pfs`, `hdl`, catalog), not frontend headers;
- PFS physical mapping goes through `ApaVolume`;
- `ps2driveforge_host` owns orchestration/export/management/enrichment policy rather than format interpretation;
- SetupAPI only discovers actual Windows devices; APA validation classifies PS2 HDDs;
- WinUI consumes native session/catalog snapshots instead of reimplementing parsing;
- Dokany sits above `ReadOnlyMountView`/`DriveSession` and does not become another parser.

If a change adds a dependency edge, document why.

## Format-note responsibilities

[`apa-format-notes.md`](apa-format-notes.md) and [`pfs-format-notes.md`](pfs-format-notes.md) should document behavior we have evidence for.

When real hardware disproves an assumption:

1. preserve the failure/sample/log where practical;
2. write a deterministic regression;
3. extend generated-image coverage if the bug crosses layers;
4. correct implementation;
5. correct format/testing/hardware docs in the same change.

## Testing responsibilities

Use the smallest appropriate layer:

- in-memory test for one invariant;
- corruption corpus for malformed metadata;
- generated `.img` E2E for cross-layer behavior/integrity;
- portable policy test for frontend contracts that do not need live Windows state;
- fuzzing to discover parser cases;
- real Windows/HDD only after deterministic gates are green.

CI cannot prove UAC, a user's storage bridge, Dokany driver state or Explorer behavior. RC hardware validation therefore remains a separate gate: [`rc-hardware-checklist.md`](rc-hardware-checklist.md).

## Performance responsibilities

Preserve repeatable baselines before changing caches, read-ahead, request ordering, Windows I/O primitives or enrichment concurrency.

A cache may memoize data that was validated under normal parser rules; it must not create a faster/weaker parse path.

Keep these timings separate:

```text
APA scan
zero-I/O catalog/list availability
optional HDL enrichment
PFS cold metadata workload
PFS warm metadata workload
large sequential payload workload
```

Never choose queue depth solely because the largest value won one completion benchmark. Consider read service latency, first-result latency, UI responsiveness and storage classification. `unknown` is a normal supported profile.

[`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md) is a reference measurement, not a universal tuning table.

## Package responsibilities

A successful compiler/linker run is not enough for Windows releases. `build-windows.ps1` creates canonical staging and [`../scripts/verify-windows-package.ps1`](../scripts/verify-windows-package.ps1) verifies required binaries/docs/tests plus the WinUI payload.

Packaging logic should discover real MSBuild output rather than encode an incidental subdirectory layout.

## Hardware-validation responsibilities

Preserve exact commit/artifact SHA, Windows/Dokany versions, device/capacity/connection, relevant APA/PFS facts, action performed, diagnostics/counters, expected/actual result and hashes where practical.

Do not publish unrelated/private disk contents just because a diagnostic listed them.

Never treat a historical `PhysicalDriveN` index as permanent device identity.

## Release-train hygiene

The repository should normally contain `main` plus the active feature/release-prep branches. Once validated work is merged, delete obsolete branches; Git history and merged PRs preserve the work.

For 0.5, the merged Emilia functionality baseline is intentionally separate from release-prep refactoring. This preserves a known-green comparison point if cleanup introduces a regression.

See [`release-process.md`](release-process.md) for RC/final gates.

## PR/commit descriptions

A useful PR records:

- problem being solved;
- layer/architectural choice;
- source-safety implications;
- tests/CI;
- real-hardware status;
- benchmark evidence if relevant;
- known limitations;
- exact remaining merge/release gate;
- next logical milestone.

Preserve the missing **why** that a source diff cannot reconstruct later.
