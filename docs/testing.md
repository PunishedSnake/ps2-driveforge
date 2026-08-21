# Testing PS2 DriveForge

DriveForge tests are split by what they actually prove. Synthetic format tests, generated-image end-to-end tests, fuzzing, CI, and real-HDD validation are complementary; none of them should be presented as a substitute for the others.

## Normal regression suite

Configure and run all normal tests with:

```bash
cmake -S . -B build -DPS2DF_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The current suite contains seven targets:

```text
ps2-driveforge-tests
ps2-driveforge-pfs-file-tests
ps2-driveforge-pfs-segi-tests
ps2-driveforge-host-tests
ps2-driveforge-e2e-image-tests
ps2-driveforge-corruption-tests
ps2-driveforge-session-tests
```

CI runs the same suite on Windows/MSVC and under Clang with AddressSanitizer + UndefinedBehaviorSanitizer + warnings-as-errors.

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

The generated image currently exercises:

- valid APA v2 MBR;
- one PFS main APA partition;
- one physically separate APA sub-partition;
- matching primary/backup PFS v3 superblocks;
- 8 KiB zones;
- empty directory preservation;
- nested directory traversal;
- small file reads;
- a 1024-byte boundary case;
- a file split across main and sub-partition extents;
- a file that requires SEGI indirect descriptor traversal;
- Windows reserved-name conversion (`CON` -> `_CON`);
- case-insensitive host collision handling (`foo` / `FOO`);
- invalid Windows host characters (`bad:name?.txt`);
- full exported-content SHA-256 verification.

This test exists specifically to catch mistakes that small in-memory `SparseDevice` fixtures cannot reveal, such as incorrect physical offsets, image-file seeking, APA extent translation, host-tree construction, and interactions between several layers.

## Corruption regression corpus

`tests/corruption_tests.cpp` deliberately builds malformed metadata and verifies that normal read-only parsing rejects it rather than trying to continue silently.

Current cases include:

- APA main extent outside the backing device;
- APA sub-partition extent outside the backing device;
- invalid PFS zone size;
- PFS root referencing a missing sub-partition;
- invalid inode checksum;
- malformed directory-entry allocation length.

When a real HDD exposes a new malformed/edge-case layout, preserve it as a minimal synthetic regression here (or in a more specific test) before fixing the parser.

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

This keeps GUI and CLI behavior testable without requiring Win32 controls or a physical disk.

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

The APA harness feeds arbitrary bytes through `apa::Reader::scan(256)`. The explicit 256-header ceiling is part of the fuzz safety contract so damaged `next` pointers cannot create an unbounded traversal.

Interesting crashes or hangs must be converted into small deterministic regression tests before a fix is considered complete.

## Performance instrumentation

The CLI can wrap the source in the same instrumentation used by `DriveSession`:

```powershell
ps2-driveforge-inspect.exe --stats --physical 3 --browse +OPL
```

Current counters include:

- APA scan count;
- PFS browse operation count;
- PFS export operation count;
- backing `BlockDevice::read()` calls;
- backing bytes requested;
- average backing-read size;
- largest backing-read size;
- failed backing reads.

These counters measure host/backend requests after PFS batching and APA translation. They are a baseline for later Emilia cache/read-ahead/overlapped-I/O work; they are not themselves proof of speed superiority.

## Windows physical-drive discovery

Before a hardware test, use:

```powershell
ps2-driveforge-inspect.exe --detect-physical
```

The discovery pass opens accessible `PhysicalDriveN` devices read-only and reports which ones contain a valid PS2 APA signature, their size, APA version, and header count.

Discovery must remain a read-only classification step. It must never turn into an automatic write-target selector.

## Real-HDD validation workflow

After all synthetic/CI gates are green, perform the hardware checks listed in [`REAL_HARDWARE_VALIDATION.md`](REAL_HARDWARE_VALIDATION.md).

For a hardware regression report, preserve:

- DriveForge version/commit;
- physical-drive index and reported size;
- exact command/action;
- complete DriveForge diagnostics;
- `--stats` output when relevant;
- whether the operation was browse-only or host export;
- expected result and actual result;
- hashes for independently verifiable exported files where practical.

Do not add write tests to the physical-HDD workflow until the project has a separate writable capability, backup/recovery design, and disposable-image destructive tests.
