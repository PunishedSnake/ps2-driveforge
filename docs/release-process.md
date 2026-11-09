# PS2 DriveForge release process

A DriveForge release has separate **code**, **package**, **legal**, **interoperability** and **real-hardware** gates. A green compiler is necessary but cannot prove Windows loader behavior, XAML resources, UAC, storage bridges, Dokany, Explorer integration, PS2 boot behavior or whether two recovery tools actually agree on the same bytes.

## Release train

Current public release: **0.5.0 - Emilia**.

Active development train: **0.6.0 - Frieren**.

Emilia remains the published read-only baseline. Frieren adds explicit image-only HDL/PFS mutation and recovery/rescue capability while keeping the normal Windows `PhysicalDrive` backend read-only.

## Gate 1 - capability and architecture safety

Required for Frieren:

- `BlockDevice` remains read-only;
- writable image operations require separate `WritableBlockDevice` / `WritableFileBlockDevice` capability;
- normal Windows `PhysicalDrive` remains `GENERIC_READ`;
- Dokany remains write-protected;
- APA/PFS/HDL/recovery logic stays below GUI/SetupAPI/UAC/Dokany layers;
- WinUI and Win32 consume shared parsers/planners instead of implementing private disk rules;
- `ApaVolume` / `WritableApaVolume` own logical-to-physical APA translation;
- normal mutation and exceptional recovery remain separate authorization domains;
- FHDB Rescue Capsule means `PS2HBRC\0` v1;
- DriveForge Mutation Journal means `PS2DFRC1` v1;
- physical write capability stays unavailable until its separate gate is completed.

A source diff containing `write()` is not evidence that the code calling it has earned permission to use it.

## Gate 2 - deterministic tests

The complete normal suite must pass on Windows/MSVC and Linux/Clang ASan+UBSan with warnings-as-errors.

The canonical Windows regression executable list is [`../scripts/frieren-regression-tests.ps1`](../scripts/frieren-regression-tests.ps1). Do not duplicate a test count here. Machine-readable manifests age more gracefully than prose integers.

Required families include:

- APA/PFS/HDL parser and corruption cases;
- image mutation and cold-reopen verification;
- OPL provider/import behavior;
- main/sub grouping and deletion ownership;
- `WriteTransaction` and Mutation Journal failure paths;
- FHDB Rescue Capsule/artifact byte-level interoperability;
- full/legacy bootstrap restore precedence and ordering;
- wrong-disk and stale-plan refusal;
- forensic recovery evidence and LBA 0 write ordering;
- Windows frontend/mount policy regressions.

The documentation/source style guard must also pass. It rejects em dashes in project-authored Markdown/C/C++ and retired Mutation Journal identifiers in executable code. Punctuation normally does not deserve CI, but ambiguous recovery terminology does.

## Gate 3 - Windows build and canonical staging

Build the complete Windows shape described in [`../BUILDING.md`](../BUILDING.md):

```powershell
.\build-windows.ps1 -Configuration Release -Clean -WithDokany -DokanyRoot '<Dokany SDK root>'
.\scripts\verify-windows-package.ps1
```

The canonical verifier must confirm native frontends/tools/tests plus the actual WinUI runtime payload. Merely locating one EXE is not package verification. Windows can fail before `wWinMain()`, a lesson already paid for once.

## Gate 4 - user package and startup

Generate clean Portable/Setup staging with `scripts/make-user-release.ps1`.

Required package behavior:

- root `PS2-DriveForge.exe` is the user-facing launcher;
- WinUI and supported Win32 fallback are staged at their expected paths;
- developer tools are separated from normal user files;
- regression executables do not leak into the user package;
- required license/credits/notices/docs are present;
- the exact staged launcher passes `--self-test` before archive creation;
- Setup and Portable artifacts are hashed.

## Gate 5 - mutation integrity

Frieren write paths must prove more than successful return codes.

For every released image mutation family, tests require the applicable sequence:

```text
validated source
 -> pure/frozen plan where practical
 -> exact bounded write set
 -> durable flush
 -> byte readback
 -> normal parser verification
 -> deterministic rollback/recovery story on failure
```

Bulk HDL install publishes APA visibility after payload verification. PFS mutations reopen through the normal reader. Known APA deletions preserve unrelated rows/data and update cached management state without pretending a rescan occurred.

## Gate 6 - FHDB Manager interoperability

DriveForge and **FHDB Manager** must agree on shared recovery artifacts and refusal policy. The FHDB Manager repository retains the historical `fhdb-bootstrap-manager` name.

The current contract is [`fhdb-bootstrap-parity.md`](fhdb-bootstrap-parity.md).

Before 0.6 sign-off, representative artifacts must round-trip both ways:

```text
FHDB Manager -> DriveForge
DriveForge    -> FHDB Manager
```

At minimum validate:

- `HDDRESCUE.BIN` / `HDDRESCUE2.BIN` `PS2HBRC\0` v1;
- `HDDMBR*` and legacy `FHDBMBR*` pointer restore input;
- `HDDRAW*`;
- `HDDMETA*` `APAMETA1` v1;
- `FORENSIC.TXT` vocabulary/schema.

Restore precedence must match FHDB Manager. A corrupt, foreign or structurally invalid full Rescue Capsule blocks legacy fallback. A valid header-only capsule may permit it. Payload is verified before pointer publication. The current master is backed up before the first restore write.

The DriveForge `PS2DFRC1` Mutation Journal is explicitly **not** part of this interchange gate.

## Gate 7 - benchmark sanity

Performance changes must preserve the basic architecture:

- one APA scan produces complete base partition data;
- catalog/group/filter/sort/select are memory-only after that scan;
- HDL enrichment remains optional/progressive;
- validated warm caches may eliminate backing reads;
- storage tuning is based on evidence rather than the personality of one benchmark disk.

The preserved Emilia baseline remains [`emilia-benchmark-2026-08-22.md`](emilia-benchmark-2026-08-22.md).

## Gate 8 - third-party and license review

Before a public release:

- DriveForge MIT license and notices are present;
- bundled/runtime dependency versions match documentation;
- third-party code/assets are not presented as DriveForge-owned;
- OPL provider behavior does not redistribute data whose license does not permit bundling;
- Windows App SDK/Dokany deployment terms match the actual shipped versions.

Historical license blockers in older RCs remain evidence, not current instructions. Review the packages that are actually being shipped rather than trusting an old paragraph because it contains many version numbers.

## Gate 9 - real Windows and real PS2 HDD

Run [`rc-hardware-checklist.md`](rc-hardware-checklist.md) against the exact candidate artifact.

The checklist must cover the applicable read path, frontend startup, UAC, APA/PFS/HDL integrity, Explorer mount lifecycle and image mutation workflows.

Frieren additionally requires disposable real-HDD/console validation for the data written by image-tested algorithms before any physical host writer can be considered. The first physical writer has a separate graduation gate and is not implied by 0.6 image support.

`PhysicalDriveN` is never release identity. Windows can renumber disks without consulting our documentation.

## Physical recovery-write graduation gate

Host physical recovery writes remain locked until all of these are proven:

1. image Rescue Capsule and legacy pointer restore are deterministic under success and injected failure;
2. wrong-disk, stale-plan, corrupt-artifact and truncated/ambiguous forensic cases fail before write;
3. mandatory FHDB safety artifacts are durable before affected metadata moves;
4. disk/volume identity and locking policy is implemented for Windows physical devices;
5. write-through/flush/readback behavior is verified across representative direct SATA and bridge paths;
6. sacrificial HDD tests reproduce successful repair and intentional refusal;
7. the resulting disk boots/reads correctly on PS2 where the operation is expected to affect boot behavior;
8. FHDB Manager can consume DriveForge-produced shared recovery artifacts from those tests.

Changing a Windows access mask is not a milestone. Proving what happens after the cache, bridge and power cable get involved is.

## Failed candidate rule

If a hardware/package/recovery candidate fails:

1. preserve exact commit and artifact hashes;
2. preserve error text/logs/screenshots and recovery artifacts;
3. reproduce at the narrowest deterministic layer possible;
4. add a regression or fault-injection case where possible;
5. issue a new candidate after the fix;
6. restart every affected gate.

Do not silently rename a failed artifact and ship it unchanged. Version strings are not healing magic.

## Frieren 0.6 sign-off

Final 0.6 requires:

- required CI green on the release commit;
- canonical/user-package verification green;
- launcher startup smoke-test green;
- exact Setup/Portable hashes recorded;
- image HDL/PFS mutation suites green with cold verification;
- recovery/refusal suites green;
- shared FHDB artifacts round-tripped across both platforms;
- real-hardware checklist passed on the exact candidate or byte-identical rebuild;
- README/changelog/living docs match what ships;
- legal/third-party review complete;
- no unresolved release blocker in format correctness, image-write integrity, recovery interoperability, mount lifecycle, frontend startup or package startup;
- physical host writes still absent unless their separate gate has truly been completed.

## Evidence to preserve

```text
version / codename / candidate
commit SHA
CI run IDs and status
Setup SHA-256
Portable SHA-256
Windows build/version
Dokany/Windows App SDK versions
source disk model/capacity/bus where relevant
APA header/main/sub counts
mutation/recovery operation and target type
shared recovery artifact hashes and producer/consumer platform
hardware checklist result
known-file/hash/boot result where applicable
benchmark report if storage policy changed
third-party/license review result
known limitations
```

The goal is an auditable release, not a ZIP that merely happened to compile and felt confident about it.
