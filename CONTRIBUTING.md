# Contributing to PS2 DriveForge

PS2 DriveForge touches raw PlayStation 2 HDD metadata. Correctness and source safety outrank convenience. The 0.5.x **Emilia** train remains read-only.

## Non-negotiable safety rules

Do not introduce source-HDD/image writes as a side effect of an unrelated feature or refactor.

Current boundaries are intentional:

- no public source `BlockDevice::write()` API;
- Windows `PhysicalDrive` opens with `GENERIC_READ`;
- Dokany mounts are write-protected and mutation/create/overwrite/delete requests are rejected;
- parsing never depends on Windows mounting the source filesystem;
- corrupted or ambiguous metadata should fail closed rather than be guessed into validity.

A future writable capability needs a separate design, explicit user intent, metadata backup/recovery and destructive tests on disposable images before any physical-HDD test.

## Architecture ownership

Keep work in the lowest correct layer:

```text
src/core       APA / PFS / HDL / portable source I/O
src/host       DriveSession / export / catalog / model / enrichment
src/platform   OS device backends and discovery
src/mount      Dokany translation/controller
src/gui        supported Win32 frontend
src/winui      WinUI 3 frontend + native-session bridge
src/launcher   user-facing Windows bootstrap/fallback
src/cli        inspector / benchmark
tests          deterministic regressions
```

Format code must not gain XAML, SetupAPI, UAC or Dokany dependencies. Frontends must not implement second copies of APA/PFS/HDL parsing. See [`docs/architecture.md`](docs/architecture.md).

## Definition of done

A meaningful parser/storage/performance/frontend change normally needs the relevant subset of:

1. implementation in the correct layer;
2. focused deterministic test;
3. generated-image E2E coverage when multiple layers interact;
4. corruption regression for malformed metadata;
5. Windows/MSVC CI;
6. Linux Clang ASan+UBSan+warnings-as-errors CI;
7. canonical and user-package verification when staging changes;
8. staged launcher smoke-test when startup/bootstrap changes;
9. real-hardware validation for UAC, SetupAPI, raw storage, Dokany, Explorer or device-dependent performance;
10. README/changelog/docs updates for changed user-visible behavior or invariants.

A typo does not need every gate. A new APA assumption does.

## Tests

The normal suite currently contains **14 executables**:

```bash
cmake -S . -B build -DPS2DF_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

See [`docs/testing.md`](docs/testing.md).

## Windows release-relevant build

```powershell
.\build-windows.ps1 `
  -Configuration Release `
  -Clean `
  -WithDokany `
  -DokanyRoot '<Dokany 2.3.1 SDK root>'

.\scripts\verify-windows-package.ps1
```

For user-facing packaging and launcher checks see [`BUILDING.md`](BUILDING.md).

Do not treat “MSBuild produced an EXE” as proof that WinUI or the launcher is deployable. Emilia has already caught both incomplete runtime staging and a loader-time ordinal import after otherwise successful builds; package/startup verifiers exist because these are distinct failure classes.

## Performance changes

Measure first and keep cold/warm cases separate. Record relevant wall time, backing reads/bytes/service time, concurrency, cache hit/miss/fill data, read-ahead useful/wasted work and base-list availability separately from optional enrichment.

Do not convert one HDD's result into a universal preset. `rotational`, `solid-state` and `unknown` are all supported states. See [`docs/performance.md`](docs/performance.md) and [`docs/emilia-benchmark-2026-08-22.md`](docs/emilia-benchmark-2026-08-22.md).

## Comments

Comments should explain **why**, units, invariants and traps rather than narrate syntax.

Useful subjects include:

- sectors vs PFS metadata blocks vs zones vs bytes;
- APA main/sub translation;
- SEGD/SEGI descriptor rules;
- PFS directory 512-byte boundary constraints;
- HDLoader allocation-table units;
- cache validity/immutability assumptions;
- conservative scheduling for unknown storage;
- NT Dokany dispositions vs Win32 creation constants;
- raw-disk `GENERIC_READ` and elevation boundaries;
- WinUI/XAML deployment/resource requirements that are not obvious from the markup.

## Real hardware

Hardware evidence should identify the exact commit/artifact, hash, Windows/Dokany version, source disk/capacity/connection, relevant APA facts, action and expected/actual result.

Never hard-code a historical `PhysicalDriveN` as identity. Windows may renumber disks. Use [`docs/rc-hardware-checklist.md`](docs/rc-hardware-checklist.md) for RC validation.

## Pull requests

A useful PR explains:

- problem being solved;
- architecture/layer choice;
- safety implications;
- tests/CI run;
- hardware status where relevant;
- performance evidence when applicable;
- known limitations;
- remaining release gate.

Keep feature work and broad cleanup separable when practical.

## Repository hygiene

Do not commit generated build trees, NuGet caches, private disk images, copied user files or irrelevant hardware logs. Prefer a small number of meaningful active branches; PRs and Git history already preserve old work.

## Licensing and provenance

Original DriveForge contributions are accepted under the repository's **MIT License** unless a contribution explicitly includes separately licensed material that has been reviewed and approved.

Before importing code, tables, assets or substantial implementation text from another project:

1. identify the exact upstream source and commit/version;
2. identify its license and attribution requirements;
3. verify compatibility with DriveForge's distribution model;
4. preserve required notices;
5. document the provenance in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) when it ships with or materially contributes code/data to DriveForge.

Using another project to understand an on-disk format is not the same as copying its implementation. Keep that distinction explicit. PS2SDK, pfsshell, hdl-dump and OPL are important compatibility/format references and are acknowledged in [`CREDITS.md`](CREDITS.md), but their independent licenses continue to apply to their own code.

Do not remove or obscure existing copyright/license notices from imported third-party material.
