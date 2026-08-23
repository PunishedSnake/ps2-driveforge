# PS2 DriveForge RC real-hardware checklist

Use this checklist against the **exact Windows candidate artifact** after deterministic CI and canonical package verification are green. Record the candidate commit and artifact SHA-256 before starting.

The normal Windows physical-disk path remains read-only. Any unexpected source mutation through `PhysicalDrive` is release-blocking.

Frieren image mutation and recovery tests are separate sections. Do not turn an image test into a physical-write experiment because the checkbox looked lonely.

## Candidate identity

```text
DriveForge version / candidate:
Commit SHA:
Setup/Portable SHA-256:
Windows version/build:
Windows App SDK / resolved WinUI version:
Dokany version:
PS2 HDD model/capacity:
Connection/bus/bridge:
PhysicalDrive index observed this run:
FHDB Manager version/commit used for interchange tests:
```

`PhysicalDriveN` is session-specific and may change after reboot or reconnection.

## A. Package and launcher

- [ ] Install Setup or unpack Portable to a clean directory.
- [ ] Confirm the package root contains the expected launcher/docs/legal/app/legacy/tools layout rather than loose runtime debris.
- [ ] Launch root `PS2-DriveForge.exe`.
- [ ] Confirm there is no loader/ordinal/missing-DLL failure before application code runs.
- [ ] Run `PS2-DriveForge.exe --legacy` and confirm the Win32 fallback starts directly.
- [ ] Confirm the exact staged launcher passes its self-test when applicable.

## B. WinUI startup and fallback

- [ ] Start root `PS2-DriveForge.exe` normally and confirm WinUI creates/activates its window.
- [ ] Confirm Fluent resources render normally.
- [ ] Confirm `%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log` contains the expected startup sequence.
- [ ] If WinUI fails, confirm the launcher offers the supported Win32 fallback instead of silently exiting.
- [ ] Confirm the fallback path can expose the startup log.

Known historical regressions that must not return include incomplete self-contained runtime staging, missing XAML resources and pre-entrypoint loader failures.

## C. Theme and readability

- [ ] Check System theme.
- [ ] Check Light theme.
- [ ] Check Dark theme.
- [ ] Confirm tree/list/status text remains readable.
- [ ] If High Contrast is available, confirm DriveForge does not force incompatible custom colors.

## D. Normal-user startup and elevation

- [ ] Launch as a normal user.
- [ ] Confirm UAC behavior does not loop.
- [ ] Accept elevation for raw read access and confirm the opened physical source remains explicitly read-only.
- [ ] Cancel elevation once and confirm image/non-admin workflows remain usable where supported.

## E. SetupAPI discovery and APA identification

- [ ] Confirm Windows disk interfaces are enumerated dynamically.
- [ ] Confirm PS2 HDD classification requires APA evidence, not model/capacity heuristics.
- [ ] If exactly one PS2 candidate exists and no source is open, confirm intended auto-open behavior.
- [ ] Record APA version, header count, main/sub counts and capacity.
- [ ] Confirm unrelated disks are not presented as PS2 HDD candidates.

Historical reference disk: APA v2, 190 headers, 43 main partitions, 147 subpartitions. Those values describe one disk, not a secret specification requirement.

## F. Partition/catalog/group behavior

- [ ] Confirm the base APA catalog becomes usable after one validated scan.
- [ ] Confirm sorting/filtering/selection does not rescan the source.
- [ ] Confirm HDL enrichment does not block base-list use.
- [ ] Confirm HDL main partitions expose owned subpartitions in a collapsible group.
- [ ] Confirm group ownership follows `main_lba`, not adjacency or child name.
- [ ] Confirm parent logical size is not double-counted when children are displayed.
- [ ] If orphan subpartitions exist in a fixture/sample, confirm they remain diagnostic rather than assigned to a convenient game.

## G. Read-only Explorer mount

- [ ] Use `Mount read-only`.
- [ ] Confirm the lowest unused letter from C: through Z: is selected.
- [ ] Confirm A: and B: are never selected automatically.
- [ ] Open the mount in Explorer.
- [ ] Browse root and `Partitions`.
- [ ] Browse known PFS locations such as `+OPL` / `__common\OPL` when present.
- [ ] Confirm normal directory opens do not reproduce the historical `The file exists` failure.

## H. Known-file integrity

Historical validation file:

```text
__common:/OPL/conf_hdd.cfg
size:   20 bytes
SHA256: E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C
```

- [ ] On the historical disk, copy through the Explorer mount and verify the hash.
- [ ] On another disk, use independently known content instead of forcing the historical value to become a religion.

## I. Physical write rejection

Attempt operations whose only acceptable result on the mounted/read-only physical source is rejection:

- [ ] create file;
- [ ] create directory;
- [ ] rename item;
- [ ] delete item;
- [ ] overwrite/truncate file.

Re-read affected source areas where practical and confirm no source metadata/content changed.

## J. Clean unmount/remount

- [ ] Unmount from the GUI.
- [ ] Confirm the drive letter disappears.
- [ ] Confirm no orphaned Dokany state blocks a second mount.
- [ ] Mount again, browse a known file and unmount cleanly.

## K. Drive-letter fallback

- [ ] Record occupied letters before mounting.
- [ ] Predict the first free C: through Z: letter and confirm DriveForge selects it.
- [ ] If practical, occupy that letter and confirm the next free letter is selected.
- [ ] Confirm A:/B: remain excluded.
- [ ] If all candidate letters are occupied, confirm mount fails without stealing one.

## L. Rescan/source preservation

- [ ] With a source open, run rescan.
- [ ] Confirm the active source is not silently replaced.
- [ ] With multiple PS2 candidates, confirm selection is explicit rather than arbitrary.
- [ ] If enrichment is running, confirm stale completion does not attach data to the new source/model.

## M. Frieren disposable-image mutation

Use disposable copies, not the physical read-only source.

- [ ] Install a PS2 ISO through the Frieren image path.
- [ ] Confirm payload is verified before APA publication.
- [ ] Cold-reopen and confirm the new APA/HDL game parses normally.
- [ ] If OPL assets are enabled, verify expected CFG/CHT/ART/TAR destinations through the normal PFS reader.
- [ ] Confirm existing CFG values survive merge policy where expected.
- [ ] Delete the installed HDL game and confirm main plus all owned subs disappear as one operation.
- [ ] Confirm unrelated partitions remain valid after deletion.

Where practical, write the resulting test image to a sacrificial PS2 HDD through an independently controlled method and confirm OPL/console behavior. Record that as console validation of DriveForge-produced bytes, not as DriveForge physical-write validation.

## N. FHDB Manager -> DriveForge interchange

On a disposable/sacrificial PS2 HDD, use **FHDB Manager** to create representative shared artifacts. The repository may still be named `fhdb-bootstrap-manager`; current product terminology is FHDB Manager.

- [ ] `HDDRESCUE.BIN` or slot 2 validates in DriveForge as `PS2HBRC\0` v1.
- [ ] Rescue metadata, APA SHA-256 and payload SHA-256 match.
- [ ] Structural KELF state/length agrees.
- [ ] Same-disk identity accepts the matching disk image and rejects a foreign one.
- [ ] `HDDMBR*` is accepted as a canonical master backup.
- [ ] Legacy `FHDBMBR*` is accepted for pointer-only restore input when valid.
- [ ] `HDDRAW*` is preserved as exact 1024-byte evidence.
- [ ] `HDDMETA*` validates as `APAMETA1` with matching entry/trailer hashes.
- [ ] `FORENSIC.TXT` is readable with the same map/evidence vocabulary.

## O. DriveForge -> FHDB Manager interchange

Generate shared artifacts from DriveForge image/recovery fixtures and place them on media readable by FHDB Manager.

- [ ] DriveForge `HDDRESCUE*` is accepted by FHDB Manager.
- [ ] DriveForge `HDDMBR*` is recognized as the same-disk master backup where applicable.
- [ ] DriveForge `HDDRAW*` is byte-identical to the captured master.
- [ ] DriveForge `HDDMETA*` is accepted/usable by the PS2-side forensic workflow where applicable.
- [ ] `FORENSIC.TXT` retains compatible vocabulary and structure.

Record producer and consumer commit/version plus SHA-256 for every artifact. Same filename without byte-level evidence is cosplay interoperability.

## P. Bootstrap restore parity on disposable images

- [ ] Full Rescue Capsule discovery prefers `HDDRESCUE.BIN`, then slot 2.
- [ ] Valid full same-disk Rescue Capsule wins over legacy backups.
- [ ] Header-only Rescue Capsule permits valid `HDDMBR*` / `FHDBMBR*` fallback.
- [ ] Corrupt, wrong-disk or invalid full Rescue Capsule blocks legacy fallback.
- [ ] Full restore saves the current `HDDMBR*` before the first target write.
- [ ] Payload is written, flushed and byte-compared before pointer publication.
- [ ] Only current `osdStart`, `osdSize` and APA checksum change in the live master.
- [ ] Master is written after payload and normal APA scan remains clean.
- [ ] Stale preflight source is refused before backup creation and before device write.
- [ ] A file beginning with DriveForge `PS2DFRC1` is rejected as an FHDB Rescue Capsule.

## Q. Legal/package check

- [ ] `LICENSE`, `CREDITS.md` and `THIRD_PARTY_NOTICES.md` are included.
- [ ] Dokany notices/source information match the actual version.
- [ ] Windows App SDK/WinUI dependency state matches current documentation.
- [ ] runtime-fetched OPL provider data is not incorrectly bundled under DriveForge MIT licensing.
- [ ] no dependency or shared recovery format is presented as DriveForge-owned when its own terms/ownership apply.

## R. Physical host-write status

For normal Frieren 0.6 validation the expected state is:

```text
[ ] PhysicalDrive remains read-only
[ ] No physical recovery writer is exposed
[ ] No physical HDL/PFS writer is exposed
```

A future physical-write candidate uses a separate destructive-hardware checklist with disk locking, exact identity, fault injection and sacrificial-media requirements. Do not improvise that checklist on a disk containing anything interesting.

## Result

```text
[ ] PASS - candidate may proceed to the next release gate
[ ] FAIL - release blocked
```

For failures, record exact step, error text/code, logs/screenshots, candidate SHA, hardware/connection, artifact hashes and whether the issue reproduces after clean restart/unmount. Preserve recovery evidence before experimenting further.
