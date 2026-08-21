# Testing PS2 DriveForge

DriveForge tests are split by what they actually prove. Synthetic format tests, generated-image end-to-end tests, fuzzing, CI, frontend-policy regressions, and real-HDD validation are complementary; none of them should be presented as a substitute for the others.

## Normal regression suite

Configure and run all normal tests with:

```bash
cmake -S . -B build -DPS2DF_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The current suite contains nine targets:

```text
ps2-driveforge-tests
ps2-driveforge-pfs-file-tests
ps2-driveforge-pfs-segi-tests
ps2-driveforge-host-tests
ps2-driveforge-e2e-image-tests
ps2-driveforge-corruption-tests
ps2-driveforge-session-tests
ps2-driveforge-dokany-open-policy-tests
ps2-driveforge-darkness-policy-tests
```

CI runs the same portable suite on Windows/MSVC and under Clang with AddressSanitizer + UndefinedBehaviorSanitizer + warnings-as-errors.

## Dokany open-policy regression

`tests/dokany_open_policy_tests.cpp` protects a filesystem-provider contract that is easy to get subtly wrong.

Dokany's `ZwCreateFile` callback receives NT kernel create-disposition values:

```text
0 FILE_SUPERSEDE
1 FILE_OPEN
2 FILE_CREATE
3 FILE_OPEN_IF
4 FILE_OVERWRITE
5 FILE_OVERWRITE_IF
```

These are not the Win32 `CreateFileW` values `CREATE_NEW`, `OPEN_EXISTING`, `OPEN_ALWAYS`, and so on. Several numeric values overlap while having different meanings.

The first real Darkness mount exposed exactly this mistake: Explorer opened the existing root using `FILE_OPEN == 1`, while the original adapter interpreted raw value `1` as Win32 `CREATE_NEW == 1`. DriveForge consequently returned `STATUS_OBJECT_NAME_COLLISION`, which Explorer surfaced as:

```text
P:\ is not accessible.
The file exists.
```

The corrected policy is isolated in `src/mount/dokany_open_policy.hpp` and is deliberately dependency-free. Both Windows/MSVC and Linux sanitizer CI verify existing-object opens, create collisions, missing-object opens, overwrite/create rejection, requested write/delete-on-close access, and directory/file type mismatches.

## Darkness GUI/mount policy regression

`tests/darkness_policy_tests.cpp` exercises the pure policy in `include/ps2hdd/darkness_policy.hpp`, protecting the small decisions around the final 0.4 GUI workflow without requiring SetupAPI, Dokany, or the runner's actual drive layout.

It verifies:

- zero detected PS2 HDDs never auto-open;
- exactly one detected PS2 HDD is the only unambiguous auto-open case when no source is already open;
- multiple PS2 HDDs require an explicit user choice;
- rescanning must not silently replace an already-open image/HDD;
- `P:` is preferred when free;
- if `P:` is occupied the policy walks `Q:` through `Z:`;
- after `P:`-`Z:` it falls back from `O:` toward `D:`;
- system letters below `D:` are never selected;
- no mount point is returned when `D:`-`Z:` are all occupied.

The Windows implementation obtains the occupied-letter mask from `GetLogicalDrives()` and follows the same documented policy. Keeping the decision pure makes future GUI/controller consolidation measurable and prevents accidental policy drift.

## Generated-image end-to-end fixture

`tests/e2e_image_tests.cpp` creates a real sparse disk-image file during the test. It does not call a special parser-only mock API after generation.

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

The image exercises valid APA v2, one PFS main plus a physically separate APA sub-partition, matching PFS v3 superblocks, 8 KiB zones, nested/empty directories, boundary reads, main/sub crossing, a SEGI-backed file, Windows-reserved names/collisions, and full exported-content SHA-256 verification.

This remains deterministic coverage for real PFS SEGI/main-sub cases that the current physical test HDD cannot provide.

## Corruption regression corpus

`tests/corruption_tests.cpp` deliberately builds malformed metadata and verifies that normal read-only parsing rejects it rather than continuing silently. Current cases include APA main/sub extents outside the device, invalid PFS zone size, root references to missing sub-partitions, invalid inode checksum, and malformed directory-entry allocation length.

When a real HDD exposes a new malformed/edge-case layout, preserve it as a minimal synthetic regression before fixing the parser.

## DriveSession tests

`tests/session_tests.cpp` verifies:

```text
BlockDevice
 -> InstrumentedBlockDevice
 -> DriveSession
      -> APA scan
      -> PFS browse
      -> host export
      -> session/backing-I/O statistics
```

This keeps GUI, CLI, and the portable mount path testable without requiring Win32 controls or a physical disk.

## Optional libFuzzer target

```bash
cmake -S . -B build/fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPS2DF_BUILD_TESTS=OFF \
  -DPS2DF_BUILD_FUZZERS=ON
cmake --build build/fuzz --target ps2-driveforge-fuzz-apa
./build/fuzz/ps2-driveforge-fuzz-apa
```

Interesting crashes/hangs must become deterministic regression tests before a fix is complete.

## Performance instrumentation and Emilia baseline

The CLI exposes `DriveSession` backing-I/O counters:

```powershell
ps2-driveforge-inspect.exe --stats --physical 3 --browse +OPL
```

Current counters include APA scans, PFS browse/export operations, backing read calls/bytes, average/largest request, and failures.

The first real-HDD `+OPL` browse baseline was **215 backing reads / 206.50 KiB**, 983 B average, 1 KiB largest, zero failures.

Before merging Darkness, preserve the final Explorer workload:

```text
cold GUI start
 -> automatic PS2 HDD discovery/open
 -> Mount read-only
 -> open in Explorer
 -> root
 -> Partitions
 -> +OPL
 -> __common\OPL
 -> read/copy conf_hdd.cfg
 -> Unmount
```

Preserve callback/debug output and any available backing-I/O totals. Emilia should repeat the same workload before/after cache, read-ahead, request-coalescing, and overlapped-I/O changes.

## Windows physical-drive discovery

Darkness no longer exposes an arbitrary `PhysicalDrive0..31` probe list. Windows discovery enumerates actual disk interfaces through SetupAPI, maps them with `IOCTL_STORAGE_GET_DEVICE_NUMBER`, then reopens the raw device through DriveForge's `GENERIC_READ` backend and lets the APA parser classify it.

Device model/capacity are presentation data, not filesystem identity.

## Standalone mount diagnostics

```powershell
.\PS2-DriveForge-Mount.exe --physical 3 --mount P: --debug 2>&1 |
  Tee-Object -FilePath darkness-mount.log
```

The standalone/shared-controller physical path has already validated root/partition browsing, known-file copy/hash, write rejection, and clean unmount.

## Final integrated-GUI hardware gate

Run the current Darkness GUI as a **normal user**.

1. Confirm one UAC relaunch and no elevation loop.
2. Confirm startup SetupAPI discovery finds the PS2 HDD with no old fixed PhysicalDrive list.
3. With exactly one candidate, confirm it opens automatically and reports ~149.05 GiB, APA v2, 43 main partitions.
4. Use `File -> Mount read-only`; `P:` should be chosen when free, otherwise another unused data letter.
5. Use `Open mounted volume in Explorer` and browse root, `Partitions`, `+OPL`, and `__common\OPL`.
6. Optionally copy `conf_hdd.cfg` and verify SHA-256 `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`.
7. Confirm create/rename/delete/write remain rejected.
8. Use the **GUI** `Unmount` command and confirm clean drive-letter removal.
9. Run `Rescan PS2 HDDs` and confirm it does not silently replace the open source.
10. Once cancel the initial UAC prompt and confirm image-file mode remains usable; `Restart as Administrator` should restore raw-disk access.
11. If practical, occupy `P:` before mount and confirm automatic fallback instead of failure.

Any functional failure here blocks merging Darkness. Win32 cosmetic polish, HDL, writable PFS, real-HDD SEGI availability, and Emilia optimization do not.

## Real-HDD validation reports

Preserve DriveForge commit/version, source device/size, exact action, diagnostics, callback/stat output when relevant, expected vs actual result, and hashes for copied/exported files where practical.

Do not add source-HDD write tests until a separate writable capability, metadata backup/recovery design, and disposable-image destructive tests exist.
