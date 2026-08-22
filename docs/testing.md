# Testing PS2 DriveForge

DriveForge tests are split by what they actually prove. Unit/synthetic format tests, generated-image end-to-end tests, corruption cases, fuzzing, CI, packaging checks, benchmarks, and real-HDD Windows validation are complementary; none should be presented as a substitute for the others.

## Normal regression suite

Configure and run the portable suite with:

```bash
cmake -S . -B build -DPS2DF_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The current normal suite contains **14 executables**:

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

Windows/MSVC CI runs the complete native suite while building WinUI and Dokany. Linux CI runs the portable suite under Clang AddressSanitizer + UndefinedBehaviorSanitizer with warnings-as-errors.

## Coverage map

| Target | Main contract |
| --- | --- |
| `ps2-driveforge-tests` | APA parsing/checksum/link and core format cases |
| `pfs-file-tests` | normal PFS files/ranges |
| `pfs-segi-tests` | SEGI indirect descriptors/large fragmented layout |
| `host-tests` | host-layer behavior |
| `e2e-image-tests` | production image -> APA -> PFS -> export -> SHA-256 |
| `corruption-tests` | malformed metadata rejection |
| `session-tests` | `DriveSession`, browse/export/stats/cache boundaries |
| `read-cache-tests` | bounded read-window semantics/counters |
| `read-ahead-tests` | sequential detection/prefetch behavior |
| `partition-catalog-tests` | zero-I/O catalog, ManagementModel, native HDL format contract |
| `hdl-enrichment-tests` | production scheduler, ordering/concurrency/cancellation policy |
| `storage-profile-tests` | storage-characteristic/tuning policy |
| `dokany-open-policy-tests` | NT create/open dispositions and write rejection |
| `darkness-policy-tests` | auto-open/source preservation/mount-letter selection |

## Generated-image end-to-end fixture

`tests/e2e_image_tests.cpp` creates a sparse disk image and reopens it through production `FileBlockDevice`:

```text
generated DriveForge-test.img
 -> FileBlockDevice
 -> APA Reader
 -> ApaVolume
 -> PFS Reader
 -> recursive host exporter
 -> exported files/directories
 -> SHA-256 comparison
```

It exercises valid APA v2, PFS v3, one main plus a physically separate APA subpartition, 8 KiB zones, nested/empty directories, boundary reads, main/sub crossing, SEGI-backed data, Windows-reserved names/collisions and full content-integrity verification.

This deterministic fixture deliberately covers PFS layouts the current physical validation HDD does not contain.

## Corruption regression corpus

`tests/corruption_tests.cpp` verifies that malformed metadata is rejected rather than partially exposed. Current cases include out-of-device APA extents, invalid PFS zone size, missing subpartitions, bad inode checksum and malformed directory allocation length.

When hardware exposes a new malformed/edge layout:

1. preserve the failure/evidence;
2. reduce it to a deterministic regression where possible;
3. extend generated-image coverage if it crosses layers;
4. fix the parser;
5. update format/testing/hardware notes in the same change.

## PFS/session cache regressions

Emilia caches validated read-only metadata within one source session. Tests protect the distinction between:

- cold cache (`clear_caches()` before the workload);
- warm workload (stats reset without clearing caches);
- backing-I/O counters below the cache layers.

A warm cache hit should remove the backing read, not merely make a recorded read faster.

## Native HDL parser and scheduler regressions

DriveForge parses HDLoader metadata in-process from the main partition at `+0x101000`, magic `0xDEADFEED`. Tests protect title/startup/compat/DMA/media/part-count/allocation-table parsing and one-read-per-game behavior.

Production enrichment is kept separate from parser correctness. Scheduler tests cover physical-LBA ordering, progressive callbacks, cancellation and bounded concurrency policy. `unknown` media must remain a valid conservative state.

## Dokany open-policy regression

Dokany's `ZwCreateFile` callback receives NT kernel create-disposition values:

```text
0 FILE_SUPERSEDE
1 FILE_OPEN
2 FILE_CREATE
3 FILE_OPEN_IF
4 FILE_OVERWRITE
5 FILE_OVERWRITE_IF
```

These are not Win32 `CreateFileW` values. The historical bug interpreted `FILE_OPEN == 1` as Win32 `CREATE_NEW == 1`, causing Explorer to show:

```text
P:\ is not accessible.
The file exists.
```

`tests/dokany_open_policy_tests.cpp` protects existing-object opens, create collisions, missing opens, overwrite/create rejection, requested write/delete-on-close access, and directory/file mismatches without needing Dokany on the Linux runner.

## Darkness discovery/mount policy regression

`tests/darkness_policy_tests.cpp` keeps Windows decisions deterministic without depending on the runner's actual disks/drive letters. It verifies:

- zero candidates never auto-open;
- exactly one PS2 HDD can auto-open only when no source is already open;
- multiple candidates require explicit selection;
- rescan never silently replaces an open source;
- `P:` preference, then `Q:`..`Z:`, then downward from `O:` to `D:`;
- letters below `D:` are never selected;
- exhaustion returns no mount point.

## Optional APA libFuzzer

```bash
cmake -S . -B build/fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPS2DF_BUILD_TESTS=OFF \
  -DPS2DF_BUILD_FUZZERS=ON
cmake --build build/fuzz --target ps2-driveforge-fuzz-apa
./build/fuzz/ps2-driveforge-fuzz-apa
```

Interesting crashes/hangs must become deterministic regressions before a parser fix is considered complete.

## Windows package smoke test

The full Windows build stages release files under `dist\windows-x64` and creates the Emilia ZIP. Verify staging with:

```powershell
.\scripts\verify-windows-package.ps1
```

The verifier checks:

- Win32 GUI;
- Dokany mount frontend;
- inspector and benchmark CLIs;
- README/CHANGELOG;
- exactly 14 packaged regression executables;
- WinUI EXE plus additional self-contained resource/runtime payload.

This check exists because a successful WinUI link does not prove a release script copied the correct MSBuild output directory.

## Emilia benchmark harness

The benchmark is read-only:

```text
ps2-driveforge-benchmark <disk-image> [--hdl] [--hdl-qd N] [--browse <partition> [path]]
ps2-driveforge-benchmark --physical <index> [--hdl] [--hdl-qd N] [--browse <partition> [path]]
```

Recommended physical-disk sweep after storage-policy changes:

```powershell
$exe = '.\ps2-driveforge-benchmark.exe'
$pd = <current PhysicalDrive index>

1..3 | % { & $exe --physical $pd }
1..3 | % { & $exe --physical $pd --browse +OPL / }
1..3 | % { & $exe --physical $pd --browse __common /OPL }
1..3 | % { & $exe --physical $pd --hdl }
foreach ($run in 1..3) {
  foreach ($qd in 1,2,4,8) {
    & $exe --physical $pd --hdl --hdl-qd $qd
  }
}
```

Do not hard-code `PhysicalDrive3` into scripts/documentation as a device identity; it was only the historical index of the validation disk.

`--hdl-qd` is a developer measurement override. Normal frontend policy uses storage characteristics plus conservative rules.

The preserved 2026-08-22 real-HDD sweep is [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md). Important measured facts on that one disk:

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

QD8 completed the narrow all-title workload ~9.6% faster than QD1 on that disk but multiplied average read service latency roughly 6.46x. The production `unknown` policy therefore intentionally stayed at QD1: base management rows were already usable and UI/first-result latency matters more than shaving ~29 ms from optional completion.

## Real-hardware Windows gate

CI cannot prove:

- UAC behavior;
- SetupAPI enumeration on the user's actual storage stack;
- raw-disk permissions through a real bridge/controller;
- Dokany driver/Explorer behavior;
- real mount-letter occupancy;
- exact WinUI startup on the target machine.

The authoritative RC sequence is [`rc-hardware-checklist.md`](rc-hardware-checklist.md). Run it only after deterministic CI/package gates are green and on the exact candidate artifact.

The historical known file on the validation HDD is:

```text
__common:/OPL/conf_hdd.cfg
size:   20 bytes
SHA256: E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C
```

Use that hash only when validating that exact file/disk. Other disks should use independently known content rather than pretending this project-specific fixture is universal.

## Evidence rule

For physical validation preserve version/commit, artifact hash, Windows/Dokany versions, source device/capacity/connection, APA characteristics, exact action, logs/counters when useful, expected vs actual result, write-protection result and hashes for copied/exported files where practical.

Do not publish irrelevant/private disk contents merely because they appeared in a diagnostic listing.
