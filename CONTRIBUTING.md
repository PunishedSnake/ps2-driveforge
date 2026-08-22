# Contributing to PS2 DriveForge

PS2 DriveForge touches raw PS2 HDD metadata, so correctness and source safety outrank convenience. The project is currently **0.5.x “Emilia”** and remains read-only.

## Non-negotiable safety rules

Do not introduce source-HDD/image writes as a side effect of an unrelated feature or refactor.

Current safety boundaries are intentional:

- no public source `BlockDevice::write()` API;
- Windows `PhysicalDrive` opens with `GENERIC_READ`;
- Dokany uses write protection and rejects mutation/create/overwrite/delete requests;
- parsing never relies on Windows filesystem mounting the source;
- corrupted/ambiguous metadata should fail closed rather than be guessed into validity.

A future writable capability needs a separate design with metadata backup/recovery, explicit user intent and destructive tests against disposable images before real HDD use.

## Architecture ownership

Keep changes in the lowest correct layer:

```text
src/core      APA / PFS / HDL / source-format and portable I/O primitives
src/host      DriveSession / export / management model / enrichment policy
src/platform  OS device backends and discovery
src/mount     Dokany translation/controller
src/gui       legacy Win32 presentation/orchestration
src/winui     WinUI 3 presentation + native-session bridge
src/cli       inspector / benchmark frontends
tests         deterministic regression coverage
```

Format code must not gain GUI, XAML, SetupAPI, UAC or Dokany dependencies. Frontends must not implement a second APA/PFS/HDL parser.

See [`docs/architecture.md`](docs/architecture.md).

## Definition of done

A meaningful parser/storage/performance/frontend change normally requires:

1. implementation in the correct layer;
2. focused deterministic test where practical;
3. generated-image E2E coverage if multiple storage layers interact;
4. corruption regression if malformed metadata is involved;
5. Windows/MSVC CI;
6. Linux Clang ASan+UBSan+warnings-as-errors CI;
7. package verification if Windows staging changes;
8. real-hardware validation when behavior depends on UAC, SetupAPI, raw Windows storage, Dokany, Explorer or device-specific performance;
9. README/changelog/docs update when user-visible behavior or an invariant changes;
10. comments explaining non-obvious units, invariants and compatibility traps.

A cosmetic typo does not need ten gates. A new APA assumption does.

## Normal test suite

The current normal suite contains **14 executables**. Run:

```bash
cmake -S . -B build -DPS2DF_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

See [`docs/testing.md`](docs/testing.md) for the target-by-target coverage map.

## Windows release build

A release-relevant Windows change should survive the same shape as CI:

```powershell
.\build-windows.ps1 `
  -Configuration Release `
  -Clean `
  -WithDokany `
  -DokanyRoot '<Dokany 2.3.1 SDK root>'

.\scripts\verify-windows-package.ps1
```

See [`BUILDING.md`](BUILDING.md).

Do not treat “MSBuild produced an EXE” as proof that the canonical ZIP contains a complete self-contained WinUI payload; the package verifier is a separate gate for exactly that reason.

## Performance changes

Measure first. Preserve cold and warm cases separately.

For caches/read-ahead/queue depth/physical I/O changes, record at least the relevant subset of:

- wall time;
- backing read calls/bytes;
- backing service time/average service;
- max reads in flight;
- small/largest request data;
- cache hit/miss/fill data;
- read-ahead useful/wasted work;
- APA/catalog/list availability separately from optional metadata enrichment.

Do not convert one HDD's result into a universal preset. `rotational`, `solid-state` and `unknown` are all supported states. The 2026-08-22 benchmark deliberately retained QD1 for unknown media even though QD8 narrowly won total HDL completion on that one disk, because QD8 greatly increased per-read latency and list availability did not depend on enrichment completion.

See [`docs/performance.md`](docs/performance.md) and [`docs/emilia-benchmark-2026-08-22.md`](docs/emilia-benchmark-2026-08-22.md).

## Comments

Comments should explain **why**, units, invariants and traps rather than narrate syntax.

Good candidates:

- sectors vs PFS metadata blocks vs zones vs bytes;
- APA main/sub translation;
- SEGD/SEGI rules;
- PFS directory 512-byte boundary constraints;
- HDLoader allocation-table units;
- why a cache contains only validated immutable data;
- why rotational/unknown scheduling is LBA ordered/conservative;
- NT Dokany `FILE_*` dispositions vs Win32 `CreateFileW` constants;
- Windows raw-disk `GENERIC_READ` and elevation boundaries.

Bad:

```cpp
// Increment offset.
offset += take;
```

Useful:

```cpp
// BlockInfo.number is a PFS zone number here, not a physical sector.
// ApaVolume performs the final APA-extent/LBA translation.
```

## Real hardware

Hardware evidence must identify the exact build/artifact. Record commit, artifact hash, Windows/Dokany versions, source disk/capacity/connection, relevant APA facts, exact action and expected/actual result.

Never hard-code a historical `PhysicalDrive3` as device identity. Windows may renumber raw disks.

For RC validation use [`docs/rc-hardware-checklist.md`](docs/rc-hardware-checklist.md).

## Pull requests

A useful PR description states:

- problem being solved;
- layer/architecture choice;
- safety implications;
- tests/CI added or run;
- hardware status where relevant;
- measured performance evidence when applicable;
- known limitations;
- exact remaining merge/release gate.

Keep feature work and broad cleanup separable when practical. The merged Emilia functionality baseline was deliberately frozen before release-prep refactoring so regressions have a known comparison point.

## Repository hygiene

Prefer one active milestone/release branch over a forest of stale branches. After validated work is merged, delete obsolete feature branches; Git history and PRs preserve the implementation history.

Do not commit generated build trees, NuGet package caches, private disk images, copied user files or irrelevant hardware logs.

## Licensing and upstream references

Format behavior may be compared against existing PS2 tools/specifications, but copied code/licensing assumptions require explicit audit. Keep implementation provenance clear and do not silently import incompatible source into DriveForge.
