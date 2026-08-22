# PS2 DriveForge documentation

This directory contains developer, format, performance and real-hardware documentation for PS2 DriveForge. The root [`README.md`](../README.md) is the user-facing overview; start here when you need implementation detail or release evidence.

## Start here

- [`../BUILDING.md`](../BUILDING.md) — Windows/Linux build prerequisites and release packaging.
- [`architecture.md`](architecture.md) — module boundaries, ownership and frontend/storage dependency rules.
- [`windows-frontends.md`](windows-frontends.md) — launcher, WinUI, supported Win32 fallback and package layout.
- [`testing.md`](testing.md) — deterministic test suite, CI and package smoke tests.
- [`release-process.md`](release-process.md) — RC/final release gates.
- [`rc-hardware-checklist.md`](rc-hardware-checklist.md) — exact real-Windows/real-PS2-HDD validation procedure.
- [`winui-diagnostics.md`](winui-diagnostics.md) — startup logging and WinUI crash triage.

## Format notes

- [`apa-format-notes.md`](apa-format-notes.md) — APA headers, checksum, linked partitions and sub-partition addressing.
- [`pfs-format-notes.md`](pfs-format-notes.md) — PFS superblock, zones, SEGD/SEGI and directory semantics.
- [`pfsshell-comparison.md`](pfsshell-comparison.md) — compatibility observations relative to pfsshell workflows.

These documents describe what DriveForge currently implements and validates. They are not independent replacements for primary PS2SDK/upstream format references.

## Emilia 0.5

- [`emilia-plan.md`](emilia-plan.md) — modernization workstreams and remaining parity work.
- [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md) — measured real-HDD baseline.
- [`performance.md`](performance.md) — cache/read-ahead/instrumentation policy.
- [`REAL_HARDWARE_VALIDATION.md`](REAL_HARDWARE_VALIDATION.md) — accumulated real-device evidence and known gaps.

## Historical planning

- [`darkness-plan.md`](darkness-plan.md) — preserved planning notes for the earlier Darkness milestone.
- [`release-codenames.md`](release-codenames.md) — release codename history.

Historical plans are retained because they explain decisions and regression coverage, but they do not override current architecture/release documentation.

## Development policy

- [`development-guidelines.md`](development-guidelines.md) — storage-safety and implementation guidelines.
- [`../CONTRIBUTING.md`](../CONTRIBUTING.md) — contribution workflow and required validation.
- [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) — dependency/reference licensing and redistribution notes.
- [`../CREDITS.md`](../CREDITS.md) — acknowledgements.

## Documentation rule

When implementation behavior changes, update the relevant documentation in the same change. In particular, release packaging, drive-letter policy, safety guarantees, test counts and frontend status must not be left describing a previous milestone.
