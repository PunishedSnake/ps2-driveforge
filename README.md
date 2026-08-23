# PS2 DriveForge

**A modern Windows toolkit for PlayStation 2 APA/PFS hard drives, with deliberately separated read, image-write and recovery capabilities.**

PS2 DriveForge lets you inspect PS2 HDDs, browse PFS filesystems, manage HDLoader game metadata, work with OPL data and perform guarded storage operations without turning a decades-old disk format into a shell-script archaeology exercise.

The current public release is **0.5.0 - Emilia**. The active **0.6.0 - Frieren** development line adds native image-only HDL/PFS mutation and an interoperable recovery/rescue layer shared with **FHDB Manager**. Physical Windows disk writes remain locked.

> [!IMPORTANT]
> The normal Windows `PhysicalDrive` backend is still read-only and uses `GENERIC_READ`. Frieren write APIs require an explicit `WritableBlockDevice`, currently backed by existing disk images. A damaged real PS2 HDD should still be imaged before host-side recovery work.

## Why DriveForge exists

Traditional PS2 HDD tooling is powerful but often assumes command-line knowledge, manual device selection and familiarity with APA/PFS internals. DriveForge puts the same formats behind a native, testable C++20 stack and conventional Windows UX.

The project now has three explicit capability domains:

```text
normal inspection        image mutation            exceptional recovery
BlockDevice              WritableBlockDevice       guarded recovery planners
PhysicalDrive read-only  WritableFileBlockDevice   FHDB-compatible artifacts
```

Keeping those separate is intentional. A browser should not acquire sector-zero privileges because somebody later added a convenient checkbox.

## Public 0.5 Emilia

Emilia established the read path:

- APA v2 master/checksum/link traversal with main/sub validation;
- PFS v3 superblock, inode, directory and SEGI reading;
- native HDLoader `0xDEADFEED` metadata parsing;
- disk-image and read-only Windows `PhysicalDriveN` backends;
- SetupAPI disk discovery followed by actual APA classification;
- recursive host export and Dokany read-only Explorer mounting;
- zero-I/O `PartitionCatalog` after one validated APA scan;
- frontend-neutral management/enrichment;
- bounded cache, read-ahead and backing-I/O instrumentation;
- WinUI 3 plus supported Win32 fallback;
- real-PS2-HDD validation and measured performance baselines.

The 0.5 release itself remains read-only. Development features below belong to Frieren until 0.6 is actually released. Version numbers are more useful when they continue to mean something.

## Frieren 0.6 development

Frieren adds explicit image-only write paths while preserving the Emilia read APIs by type.

Implemented development areas include:

- native HDL metadata patching with before-image rollback;
- zero-write APA allocation planning;
- PS2 ISO9660 and `SYSTEM.CNF` inspection;
- native HDL partition/header/DEADFEED generation;
- complete ISO-to-HDL image install with payload verification before APA publication;
- main-plus-owned-subs HDL deletion planning and apply;
- native PFS image mutation, including allocation, files, directories, SEGI/fragmented cases, copy-on-write replacement, removal and batching;
- OPL provider staging and loose/TAR PFS asset deployment;
- authoritative main/sub partition grouping for collapsible UX and logical total sizes;
- persistent DriveForge Mutation Journal for selected metadata transactions;
- FHDB-compatible Rescue Capsule and recovery artifacts;
- image-only bootstrap rescue restore and guarded APA forensic repair.

See [`docs/frieren-plan.md`](docs/frieren-plan.md) for current scope and [`docs/architecture.md`](docs/architecture.md) for capability boundaries.

## FHDB Manager interoperability

DriveForge and **FHDB Manager** are intended to be two platform-specific tools for the same PS2 HDD recovery vocabulary. FHDB Manager runs on PS2; DriveForge runs on the host. Its repository retains the historical name `fhdb-bootstrap-manager`.

Shared recovery artifacts include:

```text
HDDRESCUE.BIN / HDDRESCUE2.BIN   PS2HBRC\0 v1 Rescue Capsule
HDDMBR.BIN    / HDDMBR2.BIN      exact standard APA master backup
FHDBMBR.BIN   / FHDBMBR2.BIN     accepted legacy master-backup names
HDDRAW.BIN    / HDDRAW2.BIN      exceptional raw master snapshot
HDDMETA.BIN   / HDDMETA2.BIN     APAMETA1 touched-header backup
FORENSIC.TXT                     shared human-readable forensic report
```

The important non-shared format is:

```text
PS2DFRC1                         DriveForge Mutation Journal
```

An **FHDB Rescue Capsule** is a portable bootstrap recovery artifact. A **Mutation Journal** is a DriveForge-private transaction rollback record. They are not synonyms, aliases or two moods of the same file.

Frieren's image-only restore follows FHDB Manager semantics: validate full rescue, prove same-disk identity, validate live `__mbr` bounds, freeze and reread the current master, save `HDDMBR` before the first write, write/flush/compare payload first, publish only `osdStart`/`osdSize` plus checksum in the current master, write sector zero last and reparse APA.

A corrupt or wrong-disk full Rescue Capsule blocks legacy pointer fallback. A valid header-only capsule may fall back to `HDDMBR*` / `FHDBMBR*`, matching FHDB Manager's precedence.

The full interchange contract is [`docs/fhdb-bootstrap-parity.md`](docs/fhdb-bootstrap-parity.md).

## Safety model

DriveForge's write support is capability-based rather than a global mode:

- `BlockDevice` has no write API;
- normal `FileBlockDevice` and Windows `PhysicalDrive` are read-only;
- `WritableFileBlockDevice` is separate and only opens an existing image in place;
- `WritableApaVolume` validates logical-to-physical APA extent writes;
- `WriteTransaction` captures exact before-images and verifies write/flush/readback/rollback;
- `Mutation Journal` can persist selected small metadata transactions across interruption;
- exceptional recovery requires its own plan, source-stability checks and FHDB safety artifacts;
- physical host writes remain unavailable until their separate hardware/fault-injection gate is completed.

Recovery code is intentionally more suspicious than normal mutation code. Sector zero has earned that reputation.

## Partition and HDL UX model

APA subpartitions are no longer treated as unrelated rows merely because their IDs are unhelpful. DriveForge groups each subpartition under the main partition identified by its authoritative `main_lba` and sub number.

For HDL games this produces one collapsible logical group with a total allocation size and child extent details. Orphan subs remain diagnostic rather than being assigned by adjacency. The same main LBA is the deletion identity, so deleting one game naturally means removing its main plus owned sub headers.

## Windows release layout

The current public 0.5 distribution uses the installer or portable package. Its root remains intentionally human-readable:

```text
PS2-DriveForge.exe          default launcher
README.md
CHANGELOG.md
LICENSE
CREDITS.md
THIRD_PARTY_NOTICES.md

app\winui\                 modern frontend and runtime
legacy\                    supported Win32 fallback
tools\                     inspector / benchmark / diagnostics
docs\                      detailed documentation
```

Development Frieren packages additionally carry the current native regression/tool payload required by their build manifest. User release staging keeps internal regression executables out of normal downloads.

## WinUI diagnostics

WinUI writes an early startup log to:

```text
%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log
```

See [`docs/winui-diagnostics.md`](docs/winui-diagnostics.md).

## Mounting behavior

Read-only Explorer mounts select the lowest free drive letter from C: through Z:. A: and B: are never selected. Mounting remains read-only even on the Frieren branch.

## Performance model

The normal management path remains:

```text
one validated APA scan
 -> zero-I/O PartitionCatalog
 -> complete base management rows
 -> in-memory group/filter/sort/select
 -> optional progressive HDL/PFS enrichment
```

The 2026-08-22 Emilia benchmark measured a 190-row catalog at a median 0.006 ms after the APA scan with zero additional backing reads on one validation HDD. That is a preserved baseline, not a universal promise. See [`docs/emilia-benchmark-2026-08-22.md`](docs/emilia-benchmark-2026-08-22.md).

## Build

Windows developer/release build:

```powershell
.\build-windows.ps1 `
  -Configuration Release `
  -Clean `
  -WithDokany `
  -DokanyRoot 'C:\Program Files\Dokan\DokanLibrary-2.3.1'

.\scripts\verify-windows-package.ps1
```

Portable Linux tests:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPS2DF_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The canonical Frieren regression executable list lives in `scripts/frieren-regression-tests.ps1`. Documentation does not duplicate the count because manually synchronized integers are tiny distributed systems, and tiny distributed systems are still distributed systems.

See [`BUILDING.md`](BUILDING.md) for full prerequisites and packaging details.

## Architecture

```text
                      Windows frontends
                            |
                    ps2driveforge_host
                            |
                    ps2driveforge_core
        +------------+------+-----------+-------------+
        |            |      |           |             |
       APA          PFS    HDL     recovery/rescue   utilities
        |            |                  |
    ApaVolume  WritableApaVolume   FHDB artifacts
        |            |                  |
   BlockDevice       +---------- WritableBlockDevice
        |                              |
 image / read-only PhysicalDrive    writable image
```

Explorer/Dokany remains on the read-only side. Frontends do not reimplement APA/PFS/HDL/recovery rules.

See [`docs/architecture.md`](docs/architecture.md).

## Testing and real hardware

CI includes deterministic parser, writer, corruption, mutation-journal, FHDB interoperability, forensic-recovery, Windows policy and generated-image tests. The exact test manifest is machine-maintained rather than copied here.

A green CI build still cannot prove UAC, USB/SATA bridge behavior, Windows cache semantics, Dokany or actual PS2 boot behavior. Real-device validation therefore remains a separate gate.

Recovery interoperability also requires real artifact round trips in both directions:

```text
FHDB Manager -> DriveForge
DriveForge    -> FHDB Manager
```

See [`docs/testing.md`](docs/testing.md), [`docs/rc-hardware-checklist.md`](docs/rc-hardware-checklist.md) and [`docs/REAL_HARDWARE_VALIDATION.md`](docs/REAL_HARDWARE_VALIDATION.md).

## Current limitations

- public 0.5 Emilia remains read-only;
- Frieren mutation/recovery writes currently target disk images, not Windows physical disks;
- physical-write capability remains gated behind identity, locking, fault-injection and sacrificial-HDD validation;
- MagicGate signing of stock MBR KELF and full replacement-bootstrap installation are not yet implemented on the host;
- boot-chain evidence parity with FHDB Manager is still being expanded;
- WinUI and Win32 share core logic but some management/recovery actions still require frontend integration work;
- wider HDD/SSD/USB-bridge samples are needed before changing conservative unknown-media tuning.

## Documentation

Start with [`docs/README.md`](docs/README.md). Important current references are:

- [`docs/architecture.md`](docs/architecture.md) - component/capability ownership;
- [`docs/frieren-plan.md`](docs/frieren-plan.md) - active 0.6 plan;
- [`docs/fhdb-bootstrap-parity.md`](docs/fhdb-bootstrap-parity.md) - FHDB Manager interchange and recovery contract;
- [`docs/frieren-pfs-write.md`](docs/frieren-pfs-write.md) - native PFS mutation;
- [`docs/opl-asset-pipeline.md`](docs/opl-asset-pipeline.md) - OPL providers and import;
- [`docs/testing.md`](docs/testing.md) - regression and interoperability coverage;
- [`docs/release-process.md`](docs/release-process.md) - release gates;
- [`docs/rc-hardware-checklist.md`](docs/rc-hardware-checklist.md) - real-HDD candidate validation;
- [`docs/apa-format-notes.md`](docs/apa-format-notes.md) and [`docs/pfs-format-notes.md`](docs/pfs-format-notes.md) - format notes.

## License and acknowledgements

Original PS2 DriveForge source code is licensed under the **MIT License**. See [`LICENSE`](LICENSE).

DriveForge relies on and interoperates with independently licensed software and cross-checks format behavior against established PS2 homebrew work. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and [`CREDITS.md`](CREDITS.md).

PlayStation and related marks are trademarks of their respective owners. PS2 DriveForge is an independent homebrew project and is not affiliated with or endorsed by Sony Interactive Entertainment.
