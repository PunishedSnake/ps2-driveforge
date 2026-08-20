# Contributing to PS2 DriveForge

DriveForge is parser/filesystem code that can eventually modify real PlayStation 2 HDDs. Correctness, reproducibility and understandable design decisions matter more than making a patch look small.

## Before changing format code

Read:

- [`docs/architecture.md`](docs/architecture.md)
- [`docs/apa-format-notes.md`](docs/apa-format-notes.md)
- [`docs/pfs-format-notes.md`](docs/pfs-format-notes.md)
- [`docs/development-guidelines.md`](docs/development-guidelines.md)

If the change is motivated by pfsshell behavior or performance, also read:

- [`docs/pfsshell-comparison.md`](docs/pfsshell-comparison.md)
- [`docs/performance.md`](docs/performance.md)

## Safety

The current source-device path is read-only by design.

Do not add physical-disk writes as a convenience change. Writable support requires a separate capability, metadata backup/recovery design, destructive image tests and explicit user opt-in.

## Tests

Run a normal build and CTest locally when possible. Pull requests are also checked by:

- Windows x64 / MSVC;
- Clang + AddressSanitizer + UndefinedBehaviorSanitizer + warnings-as-errors.

A change to APA/PFS interpretation should include a synthetic regression test when practical.

## Hardware evidence

Real-HDD logs are valuable, but keep them separate from synthetic coverage. Update [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md) with what the test actually proves and any important gaps.

Do not turn one successful disk into a universal format assumption.

## Documentation is part of the patch

If a change introduces a new invariant, format discovery, performance decision, safety rule or known limitation, update the corresponding documentation in the same PR.

Add code comments at the format trap itself when a future maintainer could plausibly "simplify" the code into a bug.

## Branch hygiene

Normally keep only:

```text
main
current development/feature branch
```

After a release-train branch is validated and merged, delete it. Merged PRs and Git history preserve old work better than a pile of stale branches.

## Performance claims

Do not claim DriveForge is faster than pfsshell/pfsfuse based only on architecture or one informal copy test.

Follow [`docs/performance.md`](docs/performance.md): record versions, hardware, workload, cache state and repeated measurements.
