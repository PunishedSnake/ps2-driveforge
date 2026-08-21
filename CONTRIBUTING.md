# Contributing to PS2 DriveForge

DriveForge is parser/filesystem code that can eventually modify real PlayStation 2 HDDs. Correctness, reproducibility, safety, and understandable design decisions matter more than making a patch look small.

## Before changing format or host code

Read the relevant documentation first:

- [`docs/architecture.md`](docs/architecture.md)
- [`docs/apa-format-notes.md`](docs/apa-format-notes.md)
- [`docs/pfs-format-notes.md`](docs/pfs-format-notes.md)
- [`docs/testing.md`](docs/testing.md)
- [`docs/development-guidelines.md`](docs/development-guidelines.md)
- [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md)

For pfsshell compatibility/performance work also read:

- [`docs/pfsshell-comparison.md`](docs/pfsshell-comparison.md)
- [`docs/performance.md`](docs/performance.md)

## Safety

The current source-device path is read-only by design.

Do not add physical-disk writes as a convenience change. Writable support requires a separate capability, metadata backup/recovery design, destructive image tests, interruption/recovery testing, and explicit user opt-in.

Darkness currently has multiple independent read-only barriers:

```text
no BlockDevice::write()
PhysicalDrive = GENERIC_READ
DOKAN_OPTION_WRITE_PROTECT
mutation/create/overwrite callback rejection
```

Discovery through SetupAPI does not weaken these rules. Windows device identity only finds the actual disk object; the APA parser decides whether it is a PS2 HDD.

## Tests

Run a normal build and CTest locally when possible. Pull requests are checked by:

- Windows x64 / MSVC, including the pinned Dokany development SDK/runtime for Darkness;
- Clang + AddressSanitizer + UndefinedBehaviorSanitizer + warnings-as-errors.

The normal Darkness suite currently contains **nine test executables**, including:

- generated `.img` cross-layer E2E + SHA-256 verification;
- deterministic malformed-metadata corpus;
- DriveSession behavior/counters;
- NT Dokany open-disposition policy;
- Darkness auto-open/free-drive-letter policy.

A change to APA/PFS interpretation should include a focused synthetic regression where practical. A failure that crosses several layers belongs in the generated-image E2E fixture. Interesting fuzz/hardware failures should become deterministic regressions before the fix is considered complete.

## Hardware evidence

Real-HDD logs are valuable, but keep them separate from synthetic coverage. Update [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md) with what a test actually proves and any remaining gaps.

Do not turn one successful disk into a universal format assumption. The current physical regression HDD cannot provide a real large/fragmented PFS SEGI case, so that limitation stays explicit while generated-image coverage protects it.

Windows host changes such as SetupAPI discovery, UAC, Dokany mount lifecycle, or Explorer behavior need real-machine validation after deterministic CI is green.

## Documentation is part of the patch

If a change introduces a new invariant, format discovery, performance decision, safety rule, frontend contract, or known limitation, update the corresponding documentation in the same PR.

Add code comments at the trap itself when a future maintainer could plausibly "simplify" correct code into a bug. Good examples include:

- PFS zones vs metadata blocks;
- APA logical subparts vs physical extents;
- SEGD/SEGI self-descriptor rules;
- NT `FILE_*` vs Win32 `CreateFileW` dispositions;
- SetupAPI device enumeration vs APA filesystem identification;
- why read-only metadata may be cached but still must be validated first.

## Branch hygiene

Normally keep only:

```text
main
current development/release branch
```

After a release train is hardware-validated and merged, delete its feature branch. Merged PRs and Git history preserve old work better than stale refs.

## Performance claims

Do not claim DriveForge is faster than pfsshell/pfsfuse based on architecture or one informal copy test.

Before changing caches, batching, read-ahead, request coalescing, or Windows I/O primitives, preserve a repeatable baseline and relevant counters. Darkness' final integrated Explorer workload is the handoff baseline for 0.5 Emilia.

Use [`docs/performance.md`](docs/performance.md): record versions, hardware, workload, cold/warm state, request counters, and timed measurements where appropriate.

A faster result that weakens checksums/bounds, generated-image SHA-256 integrity, mounted read-only semantics, or real-hardware regressions is not accepted.
