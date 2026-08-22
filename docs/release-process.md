# PS2 DriveForge release process

A DriveForge release has separate **code**, **package**, **legal** and **real-hardware** gates. A green compiler is necessary but cannot prove Windows loader behavior, XAML resources, UAC, SetupAPI, Dokany, Explorer integration or a specific disk/bridge.

## Release train

Current train: **0.5.0 — Emilia**.

The Windows user package uses a small root launcher, WinUI as the default modern frontend and the proven Win32 interface as a supported fallback. Both frontends share the same native storage/host stack. Source HDD/image access remains read-only.

## Gate 1 — source safety and architecture

Required:

- physical sources open with `GENERIC_READ`;
- no public source `BlockDevice::write()` capability;
- Dokany is write-protected and mutation/create/overwrite/delete paths are rejected;
- APA/PFS/HDL logic stays below GUI/SetupAPI/UAC/Dokany layers;
- WinUI and Win32 do not implement independent format parsers;
- unknown storage profiles remain conservative instead of inheriting one test disk's tuning.

Any writable-HDD feature belongs to a later design with backup/recovery semantics and disposable-image destructive tests.

## Gate 2 — deterministic tests

The complete normal suite must pass on Windows/MSVC and Linux/Clang sanitizer CI. The current suite contains **14 regression executables** covering APA/PFS, generated-image E2E, corruption rejection, sessions/counters, caches/read-ahead, zero-I/O catalog/model behavior, native HDL enrichment, storage-profile policy, Dokany open policy and discovery/mount-letter policy.

## Gate 3 — Windows build

Build the complete Windows shape described in [`../BUILDING.md`](../BUILDING.md):

```powershell
.\build-windows.ps1 -Configuration Release -Clean -WithDokany -DokanyRoot '<Dokany 2.3.1 SDK root>'
.\scripts\verify-windows-package.ps1
```

The canonical verifier must confirm the native frontends/tools/tests plus a real WinUI runtime payload. The presence of `PS2-DriveForge-WinUI.exe` alone is insufficient.

## Gate 4 — user package and startup

Generate the clean Portable/Setup staging with `scripts/make-user-release.ps1`.

Required package behavior:

- root `PS2-DriveForge.exe` is the user-facing launcher;
- WinUI lives below `app\winui`;
- supported Win32 fallback lives below `legacy`;
- developer tools live below `tools`;
- regression test EXEs do not leak into the user package;
- `LICENSE`, `CREDITS.md` and `THIRD_PARTY_NOTICES.md` are present;
- the **exact staged launcher** executes `--self-test` successfully before archiving;
- Setup and Portable artifacts are both hashed.

The launcher smoke-test is a loader gate. It exists because RC4 produced a validly linked PE that Windows aborted before `wWinMain()` due to a common-controls ordinal import.

## Gate 5 — benchmark sanity

Performance changes must preserve Emilia's architectural contract:

- one APA scan produces the complete base partition model;
- `PartitionCatalog` construction performs zero additional source I/O;
- filter/sort/select stay memory-only;
- HDL metadata enrichment stays optional/progressive/cancellable;
- warm PFS metadata workloads can benefit from validated session caches;
- unknown/rotational media are not assigned aggressive concurrency solely from one benchmark.

Reference: [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md).

## Gate 6 — third-party/license review

Before a **public** release:

- DriveForge's MIT `LICENSE` is present;
- dependency notices are current;
- bundled Dokany version/source/license information matches the release;
- third-party code/assets are not incorrectly presented as MIT DriveForge code;
- Windows App SDK deployment is legally distributable under the actual package/runtime terms used by that build.

The current self-contained Windows App SDK 2.3.x RC path has an upstream WinUI package-license mismatch under review; see [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md). Private hardware/UI candidates may be used for testing, but this review is not optional for the final public release.

## Gate 7 — real Windows + real PS2 HDD

Run [`rc-hardware-checklist.md`](rc-hardware-checklist.md) against the exact candidate artifact.

The checklist covers:

- launcher and `--legacy` startup;
- WinUI activation/XAML resources and diagnostics;
- Win32 theme/statusbar readability;
- UAC and raw-disk discovery;
- APA identification/catalog behavior;
- Explorer mount and first-free C:–Z: drive-letter policy;
- known-file integrity;
- write rejection;
- clean unmount/remount;
- rescan/source preservation.

`PhysicalDriveN` is never part of the release identity because Windows can renumber disks.

## Failed candidate rule

If a hardware/package candidate fails:

1. preserve its exact SHA and artifact hash;
2. preserve error text/logs/screenshots;
3. reproduce at the narrowest deterministic layer possible;
4. add a regression/smoke test where possible;
5. issue a new RC number after the fix;
6. restart the affected gates.

Do not silently rename a failed artifact and ship it unchanged.

## Final 0.5.0 sign-off

Final 0.5.0 requires:

- all required CI green on the release commit;
- canonical and user-package verification green;
- launcher startup smoke-test green;
- exact Setup/Portable artifact hashes recorded;
- real-hardware checklist passed on that artifact or a byte-identical rebuild;
- README/changelog/docs reflect what actually ships;
- LICENSE/credits/third-party notices included;
- Windows App SDK public-distribution license gate resolved;
- no unresolved release-blocking issue in APA/PFS correctness, read-only safety, mount lifecycle, frontend startup or package startup.

Win32 remains supported fallback for 0.5 even if WinUI is the default frontend.

## Evidence to preserve

```text
version / codename / RC
commit SHA
CI run IDs/status
Setup SHA-256
Portable SHA-256
Windows build/version
Dokany version
source disk model/capacity/bus where relevant
APA header/main/sub counts
hardware checklist result
known-file hash result when available
benchmark report if storage policy changed
third-party/license review result
known limitations
```

The goal is an auditable release, not a ZIP that merely happened to compile.
