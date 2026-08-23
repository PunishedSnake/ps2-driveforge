# PS2 DriveForge Frieren RC real-hardware checklist

Use this checklist against the **exact Windows candidate artifact** after deterministic CI and canonical package verification are green. Record the candidate commit and artifact SHA-256 before starting.

This checklist distinguishes two Windows disk capabilities that must never be conflated:

- ordinary `PhysicalDrive` is read-only and remains the source for discovery, browsing, mounting, preflight and cold verification;
- destructive Frieren operations may obtain a separate short-lived `WritablePhysicalDrive` lease only after explicit admission, exact disk identity checks and command/UI confirmation.

Never perform the destructive sections on a disk containing unique or valuable data. A sacrificial PS2 HDD means exactly that, not "the one with 350 games but I feel lucky today".

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
Initial physical media fingerprint:
FHDB Manager version/commit used for interchange tests:
```

`PhysicalDriveN` is session-specific and may change after reboot or reconnection. The frozen media fingerprint, not the integer index, is the identity boundary.

## A. Package and launcher

- [ ] Install Setup or unpack Portable to a clean directory.
- [ ] Confirm the expected launcher/docs/legal/app/legacy/tools layout.
- [ ] Confirm `ps2-driveforge-physical-tools.exe` is present in the canonical package.
- [ ] Launch root `PS2-DriveForge.exe`.
- [ ] Confirm there is no loader/ordinal/missing-DLL failure.
- [ ] Run `PS2-DriveForge.exe --legacy` and confirm the Win32 fallback starts.
- [ ] Run the staged physical tool with `--version` and confirm PE loading/linkage succeeds without touching a raw disk.

## B. WinUI startup and fallback

- [ ] Start DriveForge normally and confirm WinUI activates its window.
- [ ] Confirm Fluent resources render normally.
- [ ] Confirm `%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log` records the startup sequence.
- [ ] If WinUI fails, confirm the launcher offers the supported Win32 fallback instead of silently exiting.

## C. Theme, responsive layout and readability

- [ ] Check System theme.
- [ ] Check Light theme.
- [ ] Check Dark theme.
- [ ] Resize to a narrow window and confirm navigation/content remain usable.
- [ ] Maximize and confirm page content uses the responsive host without clipping or broken alignment.
- [ ] Confirm tree/list/status/action text remains readable.
- [ ] If High Contrast is available, confirm DriveForge does not force incompatible custom colors.

## D. Normal-user startup and elevation

- [ ] Launch as a normal user.
- [ ] Confirm UAC behavior does not loop.
- [ ] Accept elevation for raw disk access.
- [ ] Confirm the ordinary browsing/mount source remains explicitly read-only.
- [ ] Cancel elevation once and confirm image/non-admin workflows remain usable where supported.

## E. SetupAPI discovery and APA identification

- [ ] Confirm Windows disk interfaces are enumerated dynamically.
- [ ] Confirm PS2 HDD classification requires APA evidence, not model/capacity heuristics.
- [ ] If exactly one PS2 candidate exists and no source is open, confirm intended auto-open behavior.
- [ ] Record APA version, header count, main/sub counts and capacity.
- [ ] Confirm unrelated disks are not presented as PS2 HDD candidates.

Historical reference disk: APA v2, 190 headers, 43 main partitions, 147 subpartitions. Those values describe one disk only.

## F. Partition/catalog/group behavior

- [ ] Confirm the base APA catalog becomes usable after one validated scan.
- [ ] Confirm sorting/filtering/selection does not rescan the source.
- [ ] Confirm HDL enrichment does not block base-list use.
- [ ] Confirm HDL main partitions expose owned subpartitions in a collapsible group.
- [ ] Confirm ownership follows `main_lba`, not adjacency or child name.
- [ ] Confirm parent logical size is not double-counted when children are displayed.
- [ ] Confirm orphan subpartitions remain diagnostic.

## G. Read-only Explorer mount

- [ ] Use `Mount read-only`.
- [ ] Confirm a free drive letter is selected without stealing an occupied letter.
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
- [ ] On another disk, use independently known content.

## I. Read-only barrier regression

Through the Explorer/read-only session, attempt operations whose only acceptable result is rejection:

- [ ] create file;
- [ ] create directory;
- [ ] rename item;
- [ ] delete item;
- [ ] overwrite/truncate file.

Re-read affected source areas where practical and confirm no source metadata/content changed.

This section proves that ordinary `PhysicalDrive` and Dokany remain read-only. It does **not** assert that Frieren lacks a separate guarded writer.

## J. Clean unmount/remount and rescan preservation

- [ ] Unmount from the GUI.
- [ ] Confirm the drive letter disappears.
- [ ] Confirm no orphaned Dokany state blocks a second mount.
- [ ] Mount again, browse a known file and unmount cleanly.
- [ ] With a source open, run rescan and confirm the active source is not silently replaced.
- [ ] With multiple PS2 candidates, confirm selection is explicit.
- [ ] If enrichment is running, confirm stale completion does not attach data to a new source/model.

## K. Disposable-image mutation

Use disposable copies.

- [ ] Install a PS2 ISO through the Frieren image path.
- [ ] Confirm ISO9660/SYSTEM.CNF startup validation completes before planning.
- [ ] Confirm payload is verified before APA publication.
- [ ] Cold-reopen and confirm the new APA/HDL game parses normally.
- [ ] If OPL assets are enabled, verify expected CFG/CHT/ART/TAR destinations through the normal PFS reader.
- [ ] Confirm existing CFG values survive merge policy where expected.
- [ ] Delete the installed HDL game and confirm main plus all owned subs disappear as one operation.
- [ ] Confirm unrelated partitions remain valid.
- [ ] Exercise at least one PFS create/replace/remove batch and cold-reopen it.

## L. Physical read-only admission preflight

Before **every** destructive physical test, run the read-only preflight from the same candidate package.

- [ ] `preflight <index>` opens the target through ordinary read-only `PhysicalDrive`.
- [ ] Confirm APA version/header count match the intended sacrificial disk.
- [ ] Record the returned SHA-256 media identity.
- [ ] Confirm a non-PS2 disk is refused.
- [ ] Confirm a GPT-conflicting target is refused by normal mutation admission.
- [ ] Disconnect/reconnect or otherwise change the target identity where practical and confirm a stale authorization cannot be reused.

## M. Physical confirmation barrier

For every destructive command:

- [ ] omit `--apply` and confirm the command refuses before writable access;
- [ ] provide `--apply` without `--confirm PhysicalDriveN` and confirm refusal;
- [ ] provide an incorrect `PhysicalDriveN` confirmation and confirm refusal;
- [ ] confirm the intended target only after re-reading model/capacity/fingerprint evidence;
- [ ] confirm failure happens before safety artifacts or target writes when authorization is stale or mismatched.

## N. Guarded physical HDL install

Use a sacrificial PS2 HDD and a known-good PS2 ISO.

- [ ] Create/record the required safety artifact directory before mutation.
- [ ] Run physical `install-hdl` with the exact target confirmation.
- [ ] Confirm volume locking/dismount succeeds or the operation refuses before writes.
- [ ] Confirm payload is written to unpublished free extents first.
- [ ] Confirm payload readback succeeds before APA publication.
- [ ] Confirm flush/write-through verification succeeds.
- [ ] Confirm metadata/APA publication completes last.
- [ ] Confirm the writable lease is released.
- [ ] Cold-reopen through ordinary read-only `PhysicalDrive` and confirm a clean APA/HDL parse.
- [ ] Boot the sacrificial HDD in a PS2/OPL environment and confirm the installed game is visible.
- [ ] Launch the installed game far enough to establish that the payload/layout is usable.

Record ISO hash, game ID, allocated main/sub LBAs and final parser result.

## O. Guarded physical HDL removal

- [ ] Select the game installed in section N or another disposable test game.
- [ ] Confirm grouped main/sub ownership before deletion.
- [ ] Run physical `remove` with exact target confirmation and safety artifacts.
- [ ] Confirm deletion does not zero-fill the old payload unnecessarily.
- [ ] Confirm removed extents become unreachable/free according to APA metadata.
- [ ] Confirm unrelated partitions remain byte/metadata stable where expected.
- [ ] Cold-reopen and confirm clean APA parse.
- [ ] Confirm the removed title disappears from the management model without requiring a complete unrelated HDL rescan.
- [ ] Boot the HDD in PS2/OPL and confirm the title is gone while unrelated titles remain usable.

## P. Physical PFS/OPL asset validation

When the GUI/backend exposes the physical PFS asset flow for the candidate:

- [ ] stage provider output completely on the host before write admission;
- [ ] import a small known CFG/artwork fixture;
- [ ] cold-reopen through the normal PFS reader and verify hashes/content;
- [ ] replace the same file and verify copy-on-write semantics;
- [ ] remove the fixture and verify directory/tree integrity;
- [ ] confirm failure leaves a parseable filesystem and recoverable transaction state.

If this surface is intentionally not part of the candidate, record it as **not exposed**, not silently untested.

## Q. Read-only Rescue Capsule capture

- [ ] Run `capture-rescue` without opening a writable lease.
- [ ] Confirm payload comes from the exact published `osdStart` / `osdSize` range.
- [ ] Confirm `HDDRESCUE.BIN` or slot 2 is created safely.
- [ ] Record payload bytes, geometry, hashes and KELF validity state.
- [ ] Re-run capture and confirm slot/reuse policy is deterministic.

## R. FHDB Manager -> DriveForge interchange

On sacrificial media, use FHDB Manager to create representative shared artifacts.

- [ ] `HDDRESCUE.BIN` or slot 2 validates in DriveForge as `PS2HBRC\0` v1.
- [ ] Rescue metadata, APA SHA-256 and payload SHA-256 match.
- [ ] Structural KELF state/length agrees.
- [ ] Same-disk identity accepts the matching disk and rejects a foreign disk.
- [ ] `HDDMBR*` is accepted as a canonical master backup.
- [ ] Legacy `FHDBMBR*` is accepted for pointer-only restore input when valid.
- [ ] `HDDRAW*` is preserved as exact 1024-byte evidence.
- [ ] `HDDMETA*` validates as `APAMETA1` with matching entry/trailer hashes.
- [ ] `FORENSIC.TXT` uses compatible map/evidence vocabulary.

## S. DriveForge -> FHDB Manager interchange

- [ ] DriveForge `HDDRESCUE*` is accepted by FHDB Manager.
- [ ] DriveForge `HDDMBR*` is recognized as the same-disk master backup where applicable.
- [ ] DriveForge `HDDRAW*` is byte-identical to the captured master.
- [ ] DriveForge `HDDMETA*` is accepted/usable by the PS2-side forensic workflow where applicable.
- [ ] `FORENSIC.TXT` retains compatible vocabulary and structure.

Record producer/consumer commit or version plus SHA-256 for every artifact.

## T. Guarded bootstrap restore

Use a sacrificial HDD with independently verified rescue material.

- [ ] Full Rescue Capsule discovery prefers `HDDRESCUE.BIN`, then slot 2.
- [ ] Valid full same-disk Rescue Capsule wins over legacy backups.
- [ ] Header-only Rescue Capsule permits valid `HDDMBR*` / `FHDBMBR*` fallback where specified.
- [ ] Corrupt, wrong-disk or invalid full Rescue Capsule blocks unsafe legacy fallback.
- [ ] Restore saves the current `HDDMBR*` before the first target write.
- [ ] Payload is written, flushed and byte-compared before pointer publication.
- [ ] Only intended `osdStart`, `osdSize` and required checksum bytes change in the live master.
- [ ] Sector zero/master publication occurs after payload verification.
- [ ] Writable access is released and a fresh read-only reopen confirms a clean APA chain.
- [ ] The PS2 boots the restored bootstrap where the fixture is expected to be bootable.

## U. Forensic scan and exceptional APA recovery

Corrupt a **sacrificial** fixture in a controlled, recorded way.

- [ ] `forensic-scan` works without requiring normal clean-APA admission and never opens RW access.
- [ ] Record candidate nodes/maps, inferred links, conflicts, overlaps and confidence.
- [ ] Confirm automatic repair is unavailable when evidence is ambiguous.
- [ ] Confirm manual-only recovery requires the explicit exceptional path.
- [ ] Run `repair-master` or `repair-forensic` only against a pre-recorded corruption fixture.
- [ ] Confirm `HDDRAW`, `HDDMETA` and `FORENSIC` evidence is persisted before target mutation where required.
- [ ] Confirm the frozen media identity is rechecked before commit.
- [ ] Confirm repair touches only the planned headers/master ranges.
- [ ] Cold-reopen through the canonical APA reader and compare recovered topology with the known pre-corruption fixture.

## V. Deliberate failure/recovery drills

At least one physical-write candidate must be tested under controlled failure injection before release.

- [ ] Fail before the first target write: no source mutation.
- [ ] Fail after payload write but before APA publication: no published half-installed game.
- [ ] Fail during a bounded metadata transaction: before-image/rollback or journal state is correct.
- [ ] Force readback verification failure and confirm commit is rejected.
- [ ] Present stale media identity between planning and commit and confirm refusal.
- [ ] Present wrong-disk recovery artifacts and confirm refusal before writes.
- [ ] Present `PS2DFRC1` as `HDDRESCUE.BIN` and confirm cross-format rejection.
- [ ] After each drill, preserve evidence before performing a repair.

## W. HDL Tools GUI

- [ ] The management surface is named `HDL Tools`.
- [ ] Install ISO is available for supported writable sources.
- [ ] Install preview shows startup ID/title/source/target and affected allocation before confirmation.
- [ ] Grouped HDL titles expose one logical delete action rather than raw child-partition deletion.
- [ ] Logical size includes owned subs without double counting.
- [ ] Subpartitions remain available as diagnostics.
- [ ] Physical destructive actions are visually distinct from image operations.
- [ ] Physical mutation requires explicit target confirmation and cannot bypass backend admission.
- [ ] Rescue Capsule and forensic actions use FHDB-compatible terminology.
- [ ] Result UI reports verification/cold-reopen outcome, not merely "command completed".

## X. MagicGate/bootstrap provider path

- [ ] Dedicated `MagicGate host verification` CI is green on Linux and Windows for the candidate SHA.
- [ ] Known-vector crypto/content verification passes.
- [ ] Host service fails closed when required key material is absent or inconsistent.
- [ ] Provider content is bounded, staged and provenance/SHA-256 checked before MagicGate processing.
- [ ] KELF/content inspection occurs before an immutable bootstrap install plan is produced.
- [ ] No provider/network object has direct access to a raw writer.
- [ ] Recovery artifacts are prepared before the guarded write endpoint is entered.
- [ ] Cold verification reparses the resulting bootstrap state through ordinary read-only paths.

## Y. Legal/package check

- [ ] `LICENSE`, `CREDITS.md` and `THIRD_PARTY_NOTICES.md` are included.
- [ ] Dokany notices/source information match the actual version.
- [ ] Windows App SDK/WinUI dependency state matches current documentation.
- [ ] Runtime-fetched provider data is not incorrectly bundled under DriveForge MIT licensing.
- [ ] Shared FHDB recovery formats are not presented as DriveForge-private inventions.

## Result

```text
[ ] PASS - candidate may proceed to release
[ ] FAIL - release blocked
```

For failures record exact step, error/code, logs/screenshots, candidate SHA, hardware/connection, media fingerprint, artifact hashes, before/after sector evidence where relevant and whether the issue reproduces after a clean cold reopen.
