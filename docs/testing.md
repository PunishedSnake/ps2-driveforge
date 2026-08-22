# Testing PS2 DriveForge

DriveForge tests are split by what they actually prove. Parser/unit tests, generated-image E2E, corruption cases, sanitizers, package checks, launcher startup, benchmarks and real-HDD Windows validation are complementary. None substitutes for the others.

## Normal regression suite

```bash
cmake -S . -B build -DPS2DF_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Current suite: **14 executables**.

```text
ps2-driveforge-tests
ps2-driveforge-pfs-file-tests
ps2-driveforge-pfs-segi-tests
ps2-driveforge-host-tests
ps2-driveforge-e2e-image-tests
ps2-driveforge-corruption-tests
ps2-driveforge-session-tests
ps2-driveforge-read-cache-tests
ps2-driveforge-read-ahead-tests
ps2-driveforge-partition-catalog-tests
ps2-driveforge-hdl-enrichment-tests
ps2-driveforge-storage-profile-tests
ps2-driveforge-dokany-open-policy-tests
ps2-driveforge-darkness-policy-tests
```

Windows/MSVC CI runs the complete native suite while building Dokany and WinUI. Linux CI runs portable tests under Clang ASan+UBSan with warnings-as-errors.

## Coverage map

| Target | Main contract |
| --- | --- |
| `ps2-driveforge-tests` | APA checksum/link/core format cases |
| `pfs-file-tests` | normal PFS file/range reads |
| `pfs-segi-tests` | SEGI indirect descriptors and fragmented data |
| `host-tests` | host-layer behavior |
| `e2e-image-tests` | generated image -> APA -> PFS -> export -> SHA-256 |
| `corruption-tests` | malformed metadata rejection |
| `session-tests` | `DriveSession`, browse/export/stats/cache boundaries |
| `read-cache-tests` | bounded read-window semantics/counters |
| `read-ahead-tests` | sequential detection/prefetch policy |
| `partition-catalog-tests` | zero-I/O catalog, model and HDL format contract |
| `hdl-enrichment-tests` | ordering/concurrency/cancellation policy |
| `storage-profile-tests` | storage-characteristic/tuning policy |
| `dokany-open-policy-tests` | NT create/open dispositions and write rejection |
| `darkness-policy-tests` | auto-open/source preservation/drive-letter selection |

## Generated-image E2E

`tests/e2e_image_tests.cpp` creates and reopens a sparse fixture through production code:

```text
generated image
 -> FileBlockDevice
 -> APA Reader
 -> ApaVolume
 -> PFS Reader
 -> recursive host exporter
 -> exported files/directories
 -> SHA-256 comparison
```

It covers valid APA v2, PFS v3, a physically separate APA subpartition, 8 KiB zones, nested/empty directories, boundary reads, main/sub crossing, SEGI-backed data and Windows-safe export naming. This fixture deliberately covers layouts the current physical validation HDD does not contain.

## Corruption corpus

Malformed metadata must be rejected rather than partially exposed. Current deterministic cases include out-of-device APA extents, invalid PFS zone size, missing subpartitions, bad inode checksum and malformed directory allocation length.

When hardware exposes a useful edge case: preserve evidence, reduce it to a deterministic regression where possible, extend generated-image coverage if it crosses layers, then fix the parser.

## Cache/session regressions

Emilia caches only validated read-only metadata within one source session. Tests keep **cold cache** (`clear_caches`) distinct from **warm workload** (stats reset without clearing caches). A warm hit must eliminate the backing read, not merely make a recorded read faster.

## Native HDL regressions

DriveForge parses HDLoader metadata in-process from the main partition at `+0x101000` with magic `0xDEADFEED`. Tests protect title/startup/compat/DMA/media/part-count/allocation-table parsing and one-read-per-game behavior.

Scheduler tests separately cover physical-LBA ordering, progressive callbacks, cancellation and bounded concurrency. `unknown` storage remains a valid conservative profile.

## Dokany open-policy regression

Dokany `ZwCreateFile` receives NT create dispositions, not Win32 `CreateFileW` values. Tests protect existing-object opens, missing opens, create collisions, overwrite/create rejection, write/delete-on-close access and directory/file mismatches. This is the regression that prevents the historical Explorer `The file exists` failure.

## Discovery and drive-letter regression

`tests/darkness_policy_tests.cpp` keeps Windows decisions deterministic without relying on the CI runner's actual disks or drive letters. It verifies:

- zero candidates never auto-open;
- one PS2 HDD can auto-open only when no source is already active;
- multiple candidates require explicit selection;
- rescan never silently replaces an open source;
- automatic mounting selects the **first free letter from C: through Z:**;
- A: and B: are never selected;
- exhaustion returns no mount point.

Do not reintroduce a hard-coded preferred `P:` letter.

## Optional APA libFuzzer

```bash
cmake -S . -B build/fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPS2DF_BUILD_TESTS=OFF \
  -DPS2DF_BUILD_FUZZERS=ON
cmake --build build/fuzz --target ps2-driveforge-fuzz-apa
./build/fuzz/ps2-driveforge-fuzz-apa
```

Useful crashes/hangs become deterministic regressions before a parser fix is considered complete.

## Canonical Windows package verification

```powershell
.\scripts\verify-windows-package.ps1
```

This gate verifies the native GUI/tools, documentation, exactly 14 regression executables and the actual WinUI runtime payload. It exists because a successful WinUI link does not prove the release script staged the correct MSBuild output.

## User-package and launcher smoke test

`scripts/make-user-release.ps1` builds the clean user tree and performs a separate check. Before creating Portable/Setup artifacts it executes the **exact staged root launcher**:

```powershell
.\PS2-DriveForge.exe --self-test
```

The self-test verifies that Windows can load the launcher and that its expected WinUI/Win32 payload paths exist. This specifically guards loader-time failures that occur before `wWinMain()` and therefore cannot be caught by normal command-line/fallback code.

RC4 demonstrated why this is necessary: a common-controls ordinal import killed the launcher before argument parsing even though compilation/linking had succeeded.

User-package verification also ensures regression test EXEs do not leak into the normal download and that required legal/docs files are present.

## WinUI startup diagnostics

A successful WinUI compile is not a successful real-machine startup test. RC testing preserves `%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log` and checks XAML construction/activation.

Historical fixed failures include incomplete self-contained runtime staging and missing XAML resource dictionaries. See [`winui-diagnostics.md`](winui-diagnostics.md).

## Benchmark harness

```text
ps2-driveforge-benchmark <disk-image> [--hdl] [--hdl-qd N] [--browse <partition> [path]]
ps2-driveforge-benchmark --physical <index> [--hdl] [--hdl-qd N] [--browse <partition> [path]]
```

The preserved reference sweep is [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md). Important facts for that one disk:

```text
Cold APA scan median:          1555.022 ms / 190 reads
Zero-I/O catalog median:       0.006 ms / 190 rows
Cold +OPL browse+stat:         10 reads / 16 KiB / 41.493 ms
Warm +OPL browse+stat:         0 reads / 0.001 ms
Cold __common/OPL browse+stat: 6 reads / 15 KiB / 27.689 ms
Warm repeat:                   0 reads / 0.001 ms
HDL games:                     35/35 readable
Automatic unknown-media QD:    1
```

`--hdl-qd` is a developer measurement override, not a universal recommendation.

## Real-hardware gate

CI cannot prove UAC, the user's storage stack, raw-disk permissions, Dokany driver/Explorer behavior, drive-letter occupancy, Win32 theme rendering or WinUI startup on the target machine.

The authoritative sequence is [`rc-hardware-checklist.md`](rc-hardware-checklist.md). Run it only against an exact candidate after deterministic/package gates are green.

Historical known file on the validation HDD:

```text
__common:/OPL/conf_hdd.cfg
size:   20 bytes
SHA256: E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C
```

Use that hash only for that exact known file/disk; other disks require independently known content.

## Evidence rule

For physical validation preserve version/commit, artifact hash, Windows/Dokany versions, source device/capacity/connection, APA facts, exact action, useful logs/counters, expected vs actual result, write-protection result and file hashes where practical. Do not publish unrelated/private disk contents merely because they appeared in a diagnostic listing.
