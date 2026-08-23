# PS2 DriveForge documentation

This directory contains developer, format, recovery, performance and real-hardware documentation for PS2 DriveForge. The root [`README.md`](../README.md) is the user-facing overview. Start here when you need implementation detail, safety contracts or evidence rather than another marketing adjective.

## Current architecture and development

- [`architecture.md`](architecture.md) - module boundaries, read/write capabilities, recovery trust domains and frontend/storage dependency rules.
- [`frieren-plan.md`](frieren-plan.md) - current 0.6 Frieren scope and release gates.
- [`frieren-pfs-write.md`](frieren-pfs-write.md) - native PFS mutation design and implemented limits.
- [`opl-asset-pipeline.md`](opl-asset-pipeline.md) - provider staging, CFG/CHT/artwork behavior and OPL destination pipeline.
- [`development-guidelines.md`](development-guidelines.md) - implementation, comment, documentation and storage-safety policy.
- [`testing.md`](testing.md) - deterministic suite, sanitizers, interoperability tests and hardware gates.
- [`windows-frontends.md`](windows-frontends.md) - launcher, WinUI, Win32 fallback and package layout.

## Recovery and FHDB Manager interoperability

- [`fhdb-bootstrap-parity.md`](fhdb-bootstrap-parity.md) - authoritative DriveForge/FHDB Manager vocabulary, wire formats, restore precedence, safety ordering and parity gates.
- [`apa-format-notes.md`](apa-format-notes.md) - APA headers, linked topology, sub-partition ownership and addressing.
- [`REAL_HARDWARE_VALIDATION.md`](REAL_HARDWARE_VALIDATION.md) - accumulated real-device evidence and unresolved hardware gaps.
- [`rc-hardware-checklist.md`](rc-hardware-checklist.md) - exact candidate validation procedure before release or physical-write graduation.

The PS2-side tool is called **FHDB Manager** in current documentation. Its repository is still named `fhdb-bootstrap-manager` for historical reasons. The two tools are intended to share recovery artifacts and safety semantics across console and PC.

Keep one distinction tattooed onto the mental model, preferably less painfully than an actual tattoo:

```text
FHDB Rescue Capsule = PS2HBRC\0 v1 = portable bootstrap recovery artifact
Mutation Journal     = PS2DFRC1 v1  = DriveForge-private transaction rollback record
```

They are not alternate names for the same thing.

## Format and compatibility notes

- [`apa-format-notes.md`](apa-format-notes.md) - APA structure and evidence-backed invariants.
- [`pfs-format-notes.md`](pfs-format-notes.md) - PFS superblock, zones, inode/SEGD/SEGI and directory semantics.
- [`pfsshell-comparison.md`](pfsshell-comparison.md) - compatibility observations relative to pfsshell workflows.

These documents describe behavior DriveForge currently understands or has evidence for. They do not replace primary PS2SDK/upstream references, because copying a structure definition twice does not make either copy primary.

## Performance and validation

- [`performance.md`](performance.md) - cache, read-ahead, instrumentation and measurement policy.
- [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md) - preserved real-HDD read-path baseline from Emilia.
- [`REAL_HARDWARE_VALIDATION.md`](REAL_HARDWARE_VALIDATION.md) - real Windows/PS2 HDD results.
- [`release-process.md`](release-process.md) - RC/final gates and package evidence.
- [`winui-diagnostics.md`](winui-diagnostics.md) - WinUI startup logging and crash triage.

## Historical planning

- [`emilia-plan.md`](emilia-plan.md) - historical 0.5 modernization plan.
- [`darkness-plan.md`](darkness-plan.md) - earlier Darkness planning notes.
- [`release-codenames.md`](release-codenames.md) - release codename history.

Historical plans remain because they explain old decisions and regression coverage. They do not override current architecture, Frieren or recovery documentation. Git history is useful, but making every old plan pretend it predicted the future would be needlessly theatrical.

## Repository-level docs

- [`../BUILDING.md`](../BUILDING.md) - Windows/Linux prerequisites and build/package flow.
- [`../CONTRIBUTING.md`](../CONTRIBUTING.md) - contribution workflow and validation requirements.
- [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) - dependency/reference licensing and redistribution notes.
- [`../CREDITS.md`](../CREDITS.md) - acknowledgements.

## Documentation rule

When implementation behavior changes, update the relevant living documentation in the same change. In particular, recovery vocabulary, wire formats, source-safety guarantees, test manifests, package contents and frontend status must not describe an earlier milestone.

Do not hard-code counts that already have a canonical machine-readable manifest. Do not use em dashes in project documentation or comments; use a normal hyphen. Most importantly, do not let comments rename dangerous concepts for convenience. Storage software has enough folklore without deliberately cultivating more.
