# Testing PS2 DriveForge

DriveForge tests are split by what they actually prove. Parser/unit tests, generated-image E2E, corruption cases, recovery interoperability, sanitizers, package checks, launcher startup, benchmarks and real-HDD validation are complementary. None substitutes for the others, despite the recurring human temptation to call one green checkbox a methodology.

## Normal regression suite

```bash
cmake -S . -B build -DPS2DF_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The suite grows with Frieren. Do not copy its executable count into prose. The canonical Windows list is [`../scripts/frieren-regression-tests.ps1`](../scripts/frieren-regression-tests.ps1), which is shared by build staging and package verification.

Windows/MSVC CI runs the native suite while building the Windows package and WinUI path. Linux CI runs portable tests under Clang ASan+UBSan with warnings-as-errors.

## Coverage families

| Family | Main contracts |
| --- | --- |
| APA core | checksum, linked chain, bounds, allocation, publication and removal |
| PFS reader | superblock, inode, SEGD/SEGI, directory and file addressing |
| PFS writer | bitmap allocation, file create/replace, directory growth, SEGI, removal and batching |
| HDL | metadata parsing, patching, install plan, DEADFEED generation, image install and multipart cases |
| Host/session | catalog, management, caching, export, enrichment and source/session boundaries |
| OPL | provider plans, host staging, partition resolution, loose/TAR PFS import and deploy orchestration |
| Partition UX model | authoritative main/sub grouping, orphan handling and size consistency |
| Mutation safety | exact before-images, readback, rollback and persistent Mutation Journal lifecycle |
| FHDB interoperability | Rescue Capsule wire format, shared artifacts, restore precedence, same-disk checks and legacy fallback |
| Forensic recovery | raw discovery, candidate maps, repair gating, artifact-first apply and LBA 0 ordering |
| Windows policy | storage profile, Dokany open rules, theme/startup/drive selection policies |
| E2E/corruption | generated images, cold reopen and malformed metadata refusal |

## Generated-image E2E

`tests/e2e_image_tests.cpp` creates and reopens a sparse fixture through production code:

```text
generated image
 -> BlockDevice
 -> APA Reader
 -> ApaVolume
 -> PFS Reader
 -> recursive host exporter
 -> exported files/directories
 -> SHA-256 comparison
```

Write-side suites use disposable synthetic or sparse writable images and then reopen them through normal read parsers. A writer is not considered correct because it can read its own output through a private shortcut. That is less a filesystem and more a secret handshake.

## Corruption and refusal coverage

Malformed metadata is rejected rather than partially exposed or quietly repaired. Deterministic cases cover APA bounds/link failures, PFS structure errors, invalid write plans, ambiguous recovery evidence and stale preflight state.

When hardware exposes a useful edge case, preserve evidence, reduce it to a deterministic regression where possible, extend generated-image coverage if it crosses layers, then fix the parser or writer.

## Cache and session regressions

Caches contain only data already validated through normal parser rules. Tests keep **cold cache** (`clear_caches`) distinct from a **warm workload** where counters reset but cached data remains. A warm hit must eliminate backing I/O rather than merely make a recorded read look heroic.

Known committed APA deletion can update the cached session/model snapshot without re-enriching every unrelated game. Tests protect surviving HDL enrichment and stale-background-result handling.

## HDL mutation regressions

HDLoader metadata is parsed in-process from the main partition at `+0x101000` with magic `0xDEADFEED`.

Write tests cover:

- bounded metadata patching and rollback;
- ISO startup inspection;
- zero-write APA allocation planning;
- main/sub header generation;
- complete image payload copy and byte verification;
- metadata/APA visibility published after payload;
- multipart installs;
- duplicate/corrupt target refusal;
- fast main-plus-owned-subs removal.

A failed publication may leave unreferenced payload bytes in free space. It must not leave a visible half-installed HDL partition.

## PFS mutation regressions

PFS mutation tests separately cover low-level extent/bitmap rules, directory changes, SEGI handling, copy-on-write replacement, tree removal and batch import.

OPL import tests then exercise higher-level ART/CFG/CHT and TAR destination behavior. Existing user CFG content is treated as merge input where appropriate rather than disposable scenery.

## Mutation Journal regressions

`PS2DFRC1` is tested as a DriveForge Mutation Journal, not as a Rescue Capsule. Coverage includes:

```text
PREPARED
 -> all-before / all-after / mixed / foreign classification
 -> exact before-image restoration where permitted
 -> COMMITTED terminal state
 -> RESTORED terminal state
```

Wrong target size, damaged records, unexpected bytes and terminal-state misuse fail closed.

## FHDB Manager interoperability regressions

The recovery suite protects the shared contract with FHDB Manager, whose repository retains the historical `fhdb-bootstrap-manager` name.

### Rescue Capsule format

`fhdb_rescue_tests` verifies the exact `PS2HBRC\0` v1 layout, SHA-256 fields, reserved bytes, payload geometry, structural KELF length and same-disk identity policy.

### Shared artifacts

`fhdb_artifacts_tests` protects two-slot `HDDMBR`, `HDDRAW`, `HDDRESCUE`, `HDDMETA` and `FORENSIC.TXT` behavior. Unrelated existing binary evidence is not overwritten. An identical or state-equivalent snapshot may be reused.

### Bootstrap restore

`fhdb_restore_tests` protects FHDB Manager restore semantics:

1. full `HDDRESCUE.BIN` / `HDDRESCUE2.BIN` is preferred;
2. same-disk identity is mandatory;
3. live `__mbr` bounds and the 4 MiB payload limit are enforced;
4. the current master is frozen during preflight and reread before apply;
5. `HDDMBR` safety backup exists before the first device write;
6. rescue payload is written, flushed and byte-verified before pointer publication;
7. only current `osdStart`, `osdSize` and checksum are changed in the master;
8. the master is written after payload and verified through the normal APA parser;
9. header-only Rescue Capsule may fall back to `HDDMBR*` / `FHDBMBR*`;
10. corrupt, wrong-disk or invalid full Rescue Capsule blocks legacy fallback;
11. a fake `HDDRESCUE.BIN` beginning with `PS2DFRC1` is rejected rather than mistaken for a Rescue Capsule;
12. a stale restore plan refuses before both target writes and safety-artifact creation.

That cross-format test is intentionally boring. Boring is the preferred personality of software restoring sector zero.

## APA forensic recovery regressions

Forensic tests separate discovery from authorization. They cover candidate-map evidence, conflicts, overlaps, truncation, checksum-corroborated narrow repairs, manual-only speculative plans and executor ordering.

Apply tests verify:

- `HDDRAW` before exceptional master repair;
- `HDDMETA` and `FORENSIC.TXT` before topology writes;
- source bytes reread before each mutation;
- non-master headers before LBA 0;
- flush/readback after each touched header;
- final touched-set verification;
- hard refusal of truncated evidence.

Do not lower a recovery threshold merely to make a fixture green. Fix the fixture if it was accidentally modeling a different confidence class. Tests are supposed to describe the safety rule, not negotiate with it.

## Dokany open-policy regression

Dokany `ZwCreateFile` receives NT create dispositions, not Win32 `CreateFileW` values. Portable tests protect existing/missing opens, creation collisions, overwrite rejection, write/delete-on-close access and directory/file mismatches.

This prevents the historical Explorer `The file exists` root-open failure and protects us from the seductive theory that all Microsoft constants with similar names must be interchangeable.

## Optional APA libFuzzer

```bash
cmake -S . -B build/fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPS2DF_BUILD_TESTS=OFF \
  -DPS2DF_BUILD_FUZZERS=ON
cmake --build build/fuzz --target ps2-driveforge-fuzz-apa
./build/fuzz/ps2-driveforge-fuzz-apa
```

Useful crashes or hangs become deterministic regressions before a parser fix is complete.

## Canonical Windows package verification

```powershell
.\scripts\verify-windows-package.ps1
```

The verifier consumes the canonical Frieren regression manifest rather than maintaining another manually counted list. This matters because a successful compiler run does not prove the package contains the test binary that was just added.

## User-package and launcher smoke test

`scripts/make-user-release.ps1` executes the exact staged root launcher before building Portable/Setup outputs:

```powershell
.\PS2-DriveForge.exe --self-test
```

The smoke test verifies that Windows can load the launcher and find its expected WinUI/Win32 payloads. Loader failures happen before normal argument handling, which is an inconvenient place for an application to discover that packaging was optimistic.

## Benchmark harness

```text
ps2-driveforge-benchmark <disk-image> [--hdl] [--hdl-qd N] [--browse <partition> [path]]
ps2-driveforge-benchmark --physical <index> [--hdl] [--hdl-qd N] [--browse <partition> [path]]
```

The preserved reference sweep is [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md). It is evidence for one disk and one date, not a universal queue-depth constitution.

## Real-hardware and cross-platform gate

CI cannot prove UAC, a user's storage bridge, raw-disk cache behavior, Dokany, Explorer, or actual PS2 boot behavior.

Recovery parity adds another requirement: shared artifacts must be round-tripped in both directions on real samples:

```text
FHDB Manager creates -> DriveForge validates/uses
DriveForge creates    -> FHDB Manager validates/uses
```

The authoritative real-device sequence is [`rc-hardware-checklist.md`](rc-hardware-checklist.md). Physical recovery writes on the PC remain gated even while the equivalent image path is tested.

## Evidence rule

For physical validation preserve version/commit, artifact hashes, Windows/Dokany versions, source device/capacity/connection, APA facts, exact action, useful logs/counters and expected versus actual result. For interoperability also record which platform produced and consumed each artifact.

Do not publish unrelated/private disk contents merely because they appeared in a diagnostic listing. Recovery evidence should prove the tool, not accidentally inventory the user.
