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

The corrected policy is isolated in `src/mount/dokany_open_policy.hpp` and is deliberately dependency-free. This lets both Windows/MSVC and Linux sanitizer CI verify existing-object opens, create collisions, missing-object opens, overwrite/create rejection, requested write/delete-on-close access, and directory/file type mismatches.

## Darkness GUI/mount policy regression

`tests/darkness_policy_tests.cpp` protects the small decisions around the final 0.4 GUI workflow without depending on SetupAPI, Dokany, or the runner's actual drive layout.

It verifies:

- zero detected PS2 HDDs never auto-open;
- exactly one detected PS2 HDD auto-opens only when no source is already open;
- multiple PS2 HDDs require an explicit user choice;
- rescanning cannot silently replace an already-open image/HDD;
- `P:` is preferred when free;
- if `P:` is occupied the policy walks `Q:` through `Z:`;
- after `P:`-`Z:` it falls back from `O:` toward `D:`;
- system letters below `D:` are never selected;
- no mount point is returned when `D:`-`Z:` are all occupied.

The production Windows path obtains the occupied-letter mask from `GetLogicalDrives()`. The regression keeps the decision itself deterministic so future GUI/controller refactors do not accidentally change the policy.

## Generated-image end-to-end fixture

`tests/e2e_image_tests.cpp` creates a real sparse disk-image file during the test. It does not call a special parser-only mock API after generation.

The validation path is:

```text
generated DriveForge-test.img
        -> FileBlockDevice
        -> APA Reader
        -> ApaVolume
        -> PFS Reader
        -> recursive host exporter
        -> exported files/directories
        -> SHA-256 comparison with generated source payloads
```

The generated image currently exercises valid APA v2, a PFS main partition plus physically separate APA sub-partition, matching PFS v3 superblocks, 8 KiB zones, nested/empty directories, boundary reads, main/sub crossing, a SEGI-backed file, Windows-reserved names/collisions, and full exported-content SHA-256 verification.

This remains the deterministic coverage for real PFS SEGI/main-sub cases that the current physical test HDD cannot provide.

## Corruption regression corpus

`tests/corruption_tests.cpp` deliberately builds malformed metadata and verifies that normal read-only parsing rejects it rather than trying to continue silently.

Current cases include APA main/sub extents outside the backing device, invalid PFS zone size, root references to missing sub-partitions, invalid inode checksum, and malformed directory-entry allocation length.

When a real HDD exposes a new malformed/edge-case layout, preserve it as a minimal synthetic regression before fixing the parser.

## DriveSession tests

`tests/session_tests.cpp` verifies the frontend-facing orchestration layer:

```text
BlockDevice
  -> InstrumentedBlockDevice
  -> DriveSession
       -> APA scan
       -> PFS browse
       -> host export
       -> session/backing-I/O statistics
```

This keeps GUI, CLI, and the portable part of the mount path testable without requiring Win32 controls or a physical disk.

## Optional libFuzzer target

The normal CI corpus is deterministic. For deeper parser exploration, Clang/libFuzzer can be enabled separately:

```bash
cmake -S . -B build/fuzz \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPS2DF_BUILD_TESTS=OFF \
  -DPS2DF_BUILD_FUZZERS=ON
cmake --build build/fuzz --target ps2-driveforge-fuzz-apa
./build/fuzz/ps2-driveforge-fuzz-apa
```

Interesting crashes or hangs must be converted into small deterministic regression tests before a fix is considered complete.

## Performance instrumentation and the Emilia baseline

The CLI can expose the backing-I/O counters used by `DriveSession`:

```powershell
ps2-driveforge-inspect.exe --stats --physical 3 --browse +OPL
```

Current counters include APA scans, PFS browse/export operations, backing `BlockDevice::read()` calls, bytes requested, average/largest read, and failed reads.

The first real-HDD `+OPL` browse baseline recorded under Chisato was **215 backing reads / 206.50 KiB**, with a 983 B average, 1 KiB largest request, and zero failed reads.

Before merging Darkness and starting 0.5 Emilia, preserve one Explorer workload as the mount-side baseline:

```text
cold GUI start
  -> automatic PS2 HDD discovery/open
  -> Mount read-only
  -> open mount in Explorer
  -> P:\
  -> P:\Partitions
  -> P:\Partitions\+OPL
  -> P:\Partitions\__common\OPL
  -> read/copy conf_hdd.cfg
  -> Unmount
```

For this final 0.4 validation it is enough to preserve the observable callback/debug log and any available backing-I/O totals. Emilia should compare the same workload before/after inode, directory, block cache, read-ahead, request coalescing, and overlapped physical I/O rather than relying on subjective Explorer responsiveness.

## Windows physical-drive discovery

Darkness no longer probes an arbitrary visible `PhysicalDrive0..31` list. Windows discovery enumerates actual disk interfaces through SetupAPI, maps them to their real `PhysicalDriveN` number with `IOCTL_STORAGE_GET_DEVICE_NUMBER`, then reopens the raw device through DriveForge's `GENERIC_READ` backend and lets the APA parser classify it.

Discovery is read-only classification. Device model, capacity, or Windows partition-table metadata must never become a substitute for APA validation.

## Standalone Darkness mount diagnostics

The standalone frontend remains available for scripts and callback diagnosis:

```powershell
.\PS2-DriveForge-Mount.exe --physical 3 --mount P: --debug 2>&1 |
  Tee-Object -FilePath darkness-mount.log
```

The CLI-driven real-HDD path has already validated root/partition browsing, copy/hash integrity, write rejection, and clean unmount. The remaining 0.4 hardware gate is the integrated GUI workflow below.

## Final integrated-GUI hardware gate

Run the current Darkness GUI as a **normal user**, not from an already elevated terminal.

1. Confirm DriveForge requests UAC and relaunches once as Administrator.
2. Confirm startup discovery automatically finds the PS2 APA HDD through SetupAPI; the old fixed PhysicalDrive list must not appear.
3. With exactly one PS2 HDD connected, confirm it opens automatically and reports approximately 149.05 GiB, APA v2, and 43 main partitions.
4. Use `File -> Mount read-only...`; `P:` should be chosen when free, otherwise another unused letter.
5. Use `File -> Open mounted volume in Explorer` and browse:
   ```text
   <mount>:\
   <mount>:\Partitions
   <mount>:\Partitions\+OPL
   <mount>:\Partitions\__common\OPL
   ```
6. Optionally copy `conf_hdd.cfg` again and verify SHA-256:
   ```text
   E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C
   ```
7. Confirm creating, renaming, deleting, or writing a file remains rejected.
8. Use the **GUI** `Unmount` command and confirm the drive letter disappears cleanly.
9. Run `Rescan PS2 HDDs` and confirm it does not silently replace an already-open source.
10. Once, cancel the initial UAC prompt and confirm DriveForge remains usable for opening an image; `Restart as Administrator` should recover raw-disk functionality.
11. If practical, occupy `P:` before launch and confirm the mount chooses the next free letter instead of failing.

A failure in any of these points blocks merging Darkness. Cosmetic Win32 styling, HDL support, writable PFS, real-HDD SEGI availability, and Emilia performance work do not.

## Real-HDD validation reports

For a hardware regression report preserve the DriveForge version/commit, source device/size, exact GUI action or command, complete diagnostics, callback/stat output when relevant, expected vs actual result, and hashes for independently verifiable copied/exported files.

Do not add source-HDD write tests until the project has a separate writable capability, metadata backup/recovery design, and disposable-image destructive tests.
