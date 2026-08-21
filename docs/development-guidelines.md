# Development and documentation guidelines

DriveForge should remain understandable to a contributor who did not participate in the original reverse-engineering work. Documentation is therefore part of the implementation, not cleanup to be done before 1.0.

## Definition of done for a meaningful change

A feature or refactor is not complete until the relevant items below are handled:

1. implementation;
2. focused unit/synthetic tests where practical;
3. generated-image end-to-end coverage when several storage/filesystem layers interact;
4. Windows/MSVC CI and Clang sanitizer CI;
5. hardware validation when the change depends on physical layout, Windows storage, UAC, Dokany, or Explorer behavior;
6. README update for user-visible behavior;
7. changelog update for the current release train;
8. architecture/format/performance/testing docs when an invariant or design decision changed;
9. comments next to non-obvious code;
10. known limitations recorded instead of hidden in chat/commit history.

Not every typo needs all ten steps. Anything that changes APA/PFS interpretation, I/O semantics, safety, performance strategy, discovery/mount behavior, or frontend contracts probably does.

## What deserves a code comment

Comments should explain **why**, units, invariants, and traps. Do not narrate obvious C++.

Good candidates:

- conversion between zones, metadata blocks, sectors, LBAs, and bytes;
- checks that look redundant but protect damaged metadata;
- SEGD/SEGI index rules;
- PFS directory 512-byte boundary behavior;
- APA main/sub-partition mapping;
- why a read is deliberately serialized or batched;
- Windows-specific safety constraints;
- NT Dokany `FILE_*` create dispositions vs Win32 `CreateFileW` constants;
- SetupAPI identifying the actual Windows disk while the APA parser, not Windows metadata, decides whether it is a PS2 HDD;
- future Emilia cache code memoizing **validated** metadata rather than bypassing parser validation.

Bad comment:

```cpp
// Increment offset.
offset += take;
```

Useful comment:

```cpp
// PFS BlockInfo.number is a zone number, not a sector number. Convert through
// zone_size here; ApaVolume will apply the selected APA extent's physical LBA.
```

## Comment style

Prefer comments that survive refactoring:

- state the invariant, not the current variable name;
- include units when incompatible units coexist;
- say whether a number is a format constant or a tuning value;
- mention the regression that protects a strange rule when useful;
- avoid sarcasm inside parser/storage code even if the format deserves it.

## README responsibilities

README should always answer:

- what DriveForge is;
- current version/codename;
- what works now;
- what is explicitly not implemented;
- current safety status;
- how to build/run it;
- where deeper documentation lives;
- current roadmap state and the remaining merge gate.

Do not turn README into the full PFS specification.

## Architecture responsibilities

`architecture.md` owns dependency direction and major boundaries.

Current examples:

- core parser must not depend on GUI, SetupAPI, UAC, or Dokany;
- PFS must not know Windows host paths;
- host export may know host filesystem policy but cannot mutate source PFS;
- `DriveSession` orchestrates explicit operations but must not become process-global current-directory state;
- SetupAPI/UAC/theme code belongs at the Windows host/frontend boundary;
- Dokany belongs above `ReadOnlyMountView`/`DriveSession` and must not become another parser;
- GUI and standalone mount CLI share `DokanyMountController` rather than maintaining separate callback implementations.

If a new component changes a dependency edge, update the diagram and explain why.

## Format-note responsibilities

`pfs-format-notes.md` and `apa-format-notes.md` should document only behavior we have evidence for. Link upstream references and tests where practical.

When a real disk disproves an assumption:

1. preserve the failure/log/sample where possible;
2. write a deterministic regression;
3. extend the generated-image fixture if the bug crosses layers;
4. correct the implementation;
5. correct format/testing/hardware notes in the same change.

## Testing responsibilities

[`testing.md`](testing.md) owns the concrete regression workflow.

Use the smallest appropriate level:

- focused in-memory test for one invariant;
- corruption corpus for malformed metadata;
- generated `.img` E2E for cross-layer behavior and export integrity;
- portable policy test for frontend contracts that do not need live Windows state;
- optional fuzzing to discover parser cases;
- real HDD/Windows Explorer only after deterministic CI gates are green.

The Darkness suite currently has nine normal test executables. The final integrated GUI hardware workflow remains a deliberate real-machine gate because SetupAPI/UAC/Dokany/Explorer behavior cannot be proven by a runner-only unit test.

## pfsshell comparison responsibilities

`pfsshell-comparison.md` is not marketing copy. It must distinguish:

- observed pfsshell architecture;
- implemented DriveForge behavior;
- design targets;
- measured benchmark results.

No performance adjective should quietly migrate from "target" to "fact".

## Performance responsibilities

Preserve a repeatable baseline before changing caches, batching, read-ahead, request coalescing, or Windows I/O primitives. Darkness' final integrated Explorer navigation/copy sequence is the baseline handoff to 0.5 Emilia.

A cache may memoize data that was validated under the normal parser rules; it must not create a faster, weaker parse path.

Correctness gates remain checksums/bounds, main/sub translation, SEGI, generated-image SHA-256, corruption corpus, mounted read-only behavior, CI, and hardware regressions.

## Hardware-validation responsibilities

Keep synthetic and real-hardware evidence separate. For physical validation record version/commit, device/APA/PFS characteristics when relevant, exact action, diagnostics, stats/debug output when useful, read-only status, and important coverage gaps.

Do not publish private or irrelevant user data merely because it appeared in a disk log.

## Release-train hygiene

The repository should normally contain:

```text
main
current feature/release branch
```

After a milestone is validated and merged, delete its feature branch. Git history and merged PRs preserve the work; stale branches only obscure the active state.

## Commit/PR descriptions

A useful PR records:

- problem being solved;
- architectural choice;
- safety implications;
- tests added/run;
- real-hardware status;
- known limitations;
- exact remaining merge gate;
- next logical milestone.

This preserves the missing "why" that source diffs cannot provide alone.
