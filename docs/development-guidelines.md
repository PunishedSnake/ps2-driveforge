# Development and documentation guidelines

DriveForge should remain understandable to a contributor who did not participate in the original reverse-engineering work. Documentation, tests and evidence are part of the implementation, not ceremonial cleanup scheduled for the mythical afternoon before 1.0.

## Definition of done

For meaningful storage, parser, recovery, performance or frontend changes, handle the applicable parts of:

1. implementation in the correct layer;
2. focused deterministic regression coverage;
3. generated-image E2E when several layers interact;
4. corruption/refusal coverage when malformed metadata is involved;
5. Windows/MSVC CI;
6. Linux Clang ASan+UBSan+warnings-as-errors CI;
7. canonical package verification for Windows staging changes;
8. real-hardware validation for UAC, SetupAPI, raw storage, bridges, Dokany, Explorer or device-specific behavior;
9. README, design, testing and release docs when behavior or an invariant changed;
10. comments next to non-obvious units, safety rules, format traps and cross-tool contracts.

Do not hard-code the current regression executable count into prose. `scripts/frieren-regression-tests.ps1` is the canonical Windows manifest. A duplicated number ages faster than milk in a car.

## Comment policy

Comments explain **why**, units, invariants, evidence, ownership and traps. They should not narrate C++ that can already be read one line lower.

A small amount of dry sarcasm is welcome when it helps a future contributor remember a dangerous invariant. Precision wins every argument. A joke that obscures whether a value is bytes or sectors is not documentation, it is an incident report in larval form.

Project comments and documentation use a normal hyphen `-` rather than an em dash. This keeps style consistent across source files, terminals, generated text and tooling that still treats punctuation like an exotic import.

Good candidates for comments include:

- conversion among bytes, 512-byte sectors, PFS metadata blocks and zones;
- APA main/sub extent translation and authoritative `main_lba` ownership;
- checksum/bounds checks that look redundant but reject damaged metadata;
- SEGD/SEGI traversal and allocation rules;
- PFS directory 512-byte boundary behavior;
- HDLoader allocation-table length units;
- why cache entries are accepted only after normal parser validation;
- why payload data is verified before APA visibility is published;
- why recovery writes reread frozen source bytes before commit;
- why sector zero is written last during topology repair;
- why a Rescue Capsule is not a Mutation Journal;
- Windows raw-disk access and explicit-offset OVERLAPPED rules;
- NT Dokany `FILE_*` dispositions versus Win32 `CreateFileW` constants.

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

Also useful, because humans occasionally remember the line that mildly insulted the bug:

```cpp
// Do not infer a sub-partition owner from adjacency. APA already records
// main_lba, and corruption is not a matchmaking problem.
```

Prefer comments that survive refactors. Describe the invariant instead of a temporary variable name.

## Recovery vocabulary is API policy

Recovery terminology is not cosmetic. Use these names consistently:

- **FHDB Rescue Capsule** - `PS2HBRC\0` v1, portable FHDB Manager/DriveForge bootstrap recovery artifact;
- **Mutation Journal** - `PS2DFRC1` v1, DriveForge-private before/after transaction record;
- **HDDMBR** - canonical 1024-byte current-master backup and legacy pointer source;
- **HDDRAW** - exceptional exact raw master snapshot;
- **HDDMETA** - `APAMETA1` touched-header forensic backup;
- **FORENSIC.TXT** - interoperable human-readable forensic report.

Do not introduce aliases such as `RecoveryCapsule` for the Mutation Journal. The codebase already has two real recovery concepts. It does not need a third concept whose main feature is ambiguity.

Cross-platform recovery behavior follows [`fhdb-bootstrap-parity.md`](fhdb-bootstrap-parity.md). The PS2-side tool is called **FHDB Manager** in current documentation; `fhdb-bootstrap-manager` is the historical repository name.

## README responsibilities

The root README should answer:

- what DriveForge is;
- current release and active development codename;
- what works now;
- what is intentionally image-only or read-only;
- source-safety status;
- how to build/run or where exact instructions live;
- where architecture/testing/recovery documentation lives;
- current WinUI/Win32 status without claiming parity that has not been tested.

Do not turn the README into a complete APA/PFS specification. Nobody opens a project landing page hoping for a 40-page inode seminar.

## Architecture responsibilities

[`architecture.md`](architecture.md) owns dependency direction and capability boundaries.

Current invariants include:

- `BlockDevice` remains read-only;
- `WritableBlockDevice` is a separate mutation capability;
- the existing Windows `PhysicalDrive` remains read-only;
- `ps2driveforge_core` does not depend on GUI/XAML/SetupAPI/UAC/Dokany;
- PFS physical mapping goes through `ApaVolume` / `WritableApaVolume`;
- `ps2driveforge_host` owns orchestration rather than format interpretation;
- WinUI consumes native snapshots instead of reimplementing parsing;
- Dokany remains read-only and above `DriveSession`;
- normal mutation and exceptional recovery do not share authorization;
- FHDB Rescue Capsule and DriveForge Mutation Journal remain distinct formats.

If a change adds a dependency edge or weakens a capability boundary, document why before merging it.

## Format-note responsibilities

[`apa-format-notes.md`](apa-format-notes.md) and [`pfs-format-notes.md`](pfs-format-notes.md) describe behavior for which we have evidence.

When hardware or an upstream reference disproves an assumption:

1. preserve the failure/sample/log where practical;
2. write a deterministic regression;
3. extend generated-image coverage if the bug crosses layers;
4. correct implementation;
5. correct format/testing/hardware docs in the same change.

Do not preserve a known-false assumption because an old comment sounds confident. Confidence is not a field in an APA header.

## Testing responsibilities

Use the smallest appropriate layer:

- in-memory test for one invariant;
- corruption fixture for malformed metadata;
- generated `.img` E2E for cross-layer behavior and integrity;
- portable policy test for frontend contracts that do not require live Windows state;
- cross-format tests for interoperability boundaries;
- fuzzing to discover parser cases;
- real Windows/HDD only after deterministic gates are green.

Recovery tests deserve explicit refusal cases: wrong disk, stale source, truncated scan, corrupt artifact, wrong magic, ambiguous topology and write/readback failure. A success-only recovery suite proves mostly that optimism compiles.

CI cannot prove UAC, a particular USB/SATA bridge, Dokany driver state, Explorer behavior or PS2 boot results. RC hardware validation remains a separate gate.

## FHDB Manager interoperability responsibilities

DriveForge and FHDB Manager should produce and consume the same bytes wherever a shared recovery format exists. When changing an interoperable format or policy:

1. inspect the corresponding FHDB Manager implementation, not an old note copied from it;
2. preserve exact wire identity, sizes, flag rules and filename precedence;
3. add a cross-platform fixture or byte-level test;
4. keep refusal precedence aligned, especially rescue-vs-legacy restore selection;
5. document deliberate platform differences;
6. never silently add a DriveForge-only interpretation to a shared artifact.

The desired user experience is simple: choose PS2 or PC based on where the disk is easiest to work with. Choosing a platform should not mean choosing a different definition of a valid backup.

## Mutation responsibilities

All write operations follow plan/apply separation where practical. A plan is derived from validated read state without target mutation. Apply revalidates source identity or exact frozen metadata before the first write.

For bulk-plus-metadata operations, publish visibility last. For bootstrap rescue restore, write and verify payload before publishing the pointer. For topology recovery, write interior headers before sector zero. These are format-specific rules and belong beside the format/recovery code, not hidden inside a generic write helper.

`WriteTransaction` and `MutationJournal` are primitives. They do not grant permission to mutate arbitrary storage.

## Performance responsibilities

Preserve repeatable baselines before changing caches, read-ahead, request ordering, Windows I/O primitives or enrichment concurrency.

A cache may memoize data accepted under normal parser rules. It must not create a faster/weaker parse path.

Keep these timings separate:

```text
APA scan
zero-I/O catalog/list availability
optional HDL enrichment
PFS cold metadata workload
PFS warm metadata workload
large sequential payload workload
mutation write/flush/readback
```

Never choose queue depth solely because the largest value won one completion benchmark. Consider service latency, first-result latency, UI responsiveness and storage classification. `unknown` is a normal profile.

## Package responsibilities

A successful compiler/linker run is not enough for Windows releases. `build-windows.ps1` creates canonical staging and `scripts/verify-windows-package.ps1` checks required binaries, docs, tests and WinUI payload.

`scripts/frieren-regression-tests.ps1` is the one source of truth for packaged regression executables. If a new test is wired in CMake and missing there, packaging should fail loudly instead of offering a minimalist interpretation of completeness.

## Hardware-validation responsibilities

Preserve exact commit/artifact SHA, Windows/Dokany versions, device/capacity/connection, relevant APA/PFS facts, action performed, diagnostics/counters, expected/actual result and hashes where practical.

For recovery interoperability, also preserve which tool created each artifact and which tool consumed it. Real parity means `FHDB Manager -> DriveForge` and `DriveForge -> FHDB Manager` round-trips, not two independent tests that happen to use the same filename.

Do not publish unrelated/private disk contents merely because a diagnostic listed them. Never treat a historical `PhysicalDriveN` index as permanent identity.

## Historical documents

Plans and benchmark reports tied to a past release or date remain historical evidence. Do not rewrite them to pretend they described current Frieren behavior all along. Add a clear historical note where necessary and update living documents instead.

Git already preserves history. Documentation does not need to perform historical revisionism as a side hobby.

## Release-train hygiene

The repository should normally contain `main` plus active feature/release-prep branches. Once validated work is merged, delete obsolete branches; Git history and merged PRs preserve the work.

See [`release-process.md`](release-process.md) for RC/final gates.

## PR and commit descriptions

A useful PR records:

- problem being solved;
- architectural/capability choice;
- source-safety implications;
- tests and CI;
- real-hardware status;
- interoperability impact when recovery formats are involved;
- benchmark evidence if relevant;
- known limitations;
- exact remaining merge/release gate;
- next logical milestone.

Preserve the missing **why** that a source diff cannot reconstruct later. The diff already knows which semicolon moved.
