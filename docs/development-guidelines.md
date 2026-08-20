# Development and documentation guidelines

DriveForge should remain understandable to a contributor who did not participate in the original reverse-engineering work. Documentation is therefore part of the implementation, not cleanup to be done before 1.0.

## Definition of done for a meaningful change

A feature or refactor is not complete until the relevant items below are handled:

1. implementation;
2. unit/synthetic tests where practical;
3. Windows CI and sanitizer CI;
4. hardware validation when the change touches on-disk interpretation;
5. README update if user-visible behavior changed;
6. changelog update for the current release train;
7. architecture/format/performance docs when an invariant or design decision changed;
8. comments next to non-obvious code;
9. known limitations recorded instead of hidden in chat/commit history.

Not every typo needs all nine steps. Anything that changes APA/PFS interpretation, I/O semantics, safety, performance strategy, or frontend behavior probably does.

## What deserves a code comment

Comments should explain **why**, units, invariants, and format traps. Do not narrate obvious C++.

Good candidates:

- conversion between zones, metadata blocks, sectors, LBAs, and bytes;
- checks that look redundant but protect damaged metadata;
- SEGD/SEGI index rules;
- PFS directory 512-byte boundary behavior;
- APA main/sub-partition mapping;
- why a read is deliberately serialized or batched;
- Windows-specific safety constraints;
- compatibility behavior copied from an upstream format implementation;
- a deliberate difference from pfsshell/PS2SDK semantics at the host API boundary.

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
- include units (`bytes`, `512-byte sectors`, `zones`) when two units coexist;
- say whether a number is a format constant or merely a tuning value;
- mention the test that protects a particularly strange rule when useful;
- avoid sarcasm inside format/parser code even if the format deserves it.

## README responsibilities

README is for a new user/contributor. It should always answer:

- what DriveForge is;
- current version/codename;
- what works now;
- what is explicitly not implemented;
- safety status;
- how to build/run it;
- how to find deeper documentation;
- current roadmap state.

Do not turn README into the full PFS specification. Link to developer docs instead.

## Architecture documentation responsibilities

`architecture.md` owns layer boundaries and dependency direction.

If a new component starts depending on another layer, update the diagram. Examples:

- core parser must not depend on GUI;
- PFS should not know Windows paths;
- host export may know host filesystem policy but should not mutate source PFS;
- Dokany belongs above core/host abstractions.

## Format-note responsibilities

`pfs-format-notes.md` (and future APA notes) should document only behavior we have evidence for. Link upstream references and tests when possible.

When a real disk disproves an assumption:

1. preserve the failing sample/log if possible;
2. write a regression test;
3. correct the implementation;
4. correct the format note in the same change.

## pfsshell comparison responsibilities

`pfsshell-comparison.md` is not marketing copy.

It must distinguish:

- observed pfsshell architecture;
- implemented DriveForge behavior;
- planned advantages;
- measured benchmark results.

No performance adjective should quietly migrate from "design target" to "fact".

## Hardware-validation responsibilities

Keep synthetic tests and real-hardware observations separate.

For physical HDD validation record:

- DriveForge version/commit when known;
- device size and APA version;
- PFS version/zone size when relevant;
- exact operation used;
- successful observations;
- errors/warnings;
- whether the test was read-only;
- anything unusual in the disk layout that makes it valuable as a regression case.

Do not publish private or irrelevant user data just because it appeared in a disk log.

## Release-train hygiene

The repository should normally contain:

```text
main
current feature/release branch
```

After a release-train milestone is validated and merged, delete its feature branch. Git history and merged PRs preserve the work; stale branches only make the active state harder to understand.

## Commit/PR descriptions

A useful PR should contain:

- problem being solved;
- architectural choice;
- safety implications;
- tests added/run;
- real-hardware status;
- known limitations;
- next logical milestone.

This gives future contributors the missing "why" that source diffs cannot provide alone.
