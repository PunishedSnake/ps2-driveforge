# Emilia 0.5 RC real-hardware checklist

Use this checklist on the **exact Windows candidate artifact** after all deterministic CI gates are green. Record the candidate commit and ZIP SHA-256 before starting.

This is a read-only validation. A failure to reject source mutation is release-blocking.

## Candidate identity

Record:

```text
DriveForge commit:
Candidate ZIP SHA-256:
Windows version:
Dokany version:
PS2 HDD model/capacity:
Connection/bus/bridge:
PhysicalDrive index observed this run:
```

Do not treat the current `PhysicalDriveN` number as stable across reboot/reconnection.

## A. Normal-user startup and elevation

- [ ] Launch `PS2-DriveForge.exe` as a normal, non-elevated user.
- [ ] Confirm exactly one UAC elevation request for raw-disk access; there must be no relaunch loop.
- [ ] Accept UAC and confirm the elevated instance starts normally.
- [ ] Confirm the UI clearly remains read-only.

## B. SetupAPI discovery and APA identification

- [ ] Confirm startup discovers actual Windows disk interfaces rather than presenting a fixed `PhysicalDrive0..31` list.
- [ ] Confirm the PS2 HDD is identified by a successful APA scan, not merely by Windows model/capacity metadata.
- [ ] If exactly one PS2 HDD candidate exists and no image/source is already open, confirm it opens automatically.
- [ ] Record APA version, total header count, main/sub counts and capacity.
- [ ] Confirm unrelated disks are not exposed as PS2 HDD candidates.

Reference validation disk historically reports APA v2, 190 headers, 43 main partitions and 147 sub-partitions; those numbers are **not** universal requirements for other disks.

## C. Partition/catalog behavior

- [ ] Confirm the usable APA partition list appears immediately after the one APA scan.
- [ ] Confirm sorting/filtering/selection does not trigger a new disk scan or modal per-game initialization.
- [ ] Confirm HDL titles/details may enrich progressively without blocking base-list use.
- [ ] Change source/rescan during optional enrichment if practical and confirm stale enrichment does not corrupt the new model.

## D. Read-only Explorer mount

- [ ] Use the GUI `Mount read-only` action.
- [ ] Confirm DriveForge selects the **lowest currently unused drive letter from C: through Z:**.
- [ ] Confirm A: and B: are never selected automatically, even when they are unused.
- [ ] Use `Open mounted volume in Explorer`.
- [ ] Browse the mounted root.
- [ ] Browse `Partitions`.
- [ ] Browse a known PFS partition such as `+OPL`.
- [ ] Browse `__common\OPL` when present.
- [ ] Confirm normal directory opens do not produce the historical `The file exists` error.

## E. Known-file integrity

On the project's historical validation disk, copy:

```text
__common:/OPL/conf_hdd.cfg
```

Expected historical result:

```text
size:   20 bytes
SHA256: E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C
```

- [ ] If that exact file exists on this disk, copy it through the mounted Explorer path and verify the hash.
- [ ] If using another disk, copy a known file and compare against an independently known source hash/content instead of forcing the historical hash.

## F. Write rejection

Attempt only harmless operations whose expected result is rejection:

- [ ] create a new file in the mounted volume — rejected;
- [ ] create a directory — rejected;
- [ ] rename an existing item — rejected;
- [ ] delete an existing item — rejected;
- [ ] overwrite/truncate an existing file — rejected.

After the attempts, re-read/browse the affected source area and confirm no source metadata/content changed.

## G. Clean unmount and remount

- [ ] Use the **GUI** `Unmount` action.
- [ ] Confirm the drive letter disappears cleanly.
- [ ] Confirm there is no orphaned Dokany mount/process state preventing another mount.
- [ ] Mount again and browse at least one known partition/file.
- [ ] Unmount again cleanly.

## H. Rescan/source preservation

- [ ] With a source open, run `Rescan PS2 HDDs`.
- [ ] Confirm rescan does not silently replace the currently open image/HDD.
- [ ] If multiple PS2 candidates exist, confirm the UI requires an explicit choice rather than arbitrary auto-open.

## I. UAC cancellation and image-only mode

- [ ] Start again as a normal user and cancel the initial UAC prompt once.
- [ ] Confirm the program remains usable for disk-image workflows instead of exiting or looping.
- [ ] Confirm raw physical-drive access is unavailable/limited as expected.
- [ ] Use `Restart as Administrator` and confirm raw-disk discovery/access is restored.

## J. Mount-letter fallback

- [ ] Record the letters already occupied in Windows before mounting.
- [ ] Predict the first free letter in the C: through Z: range and confirm DriveForge takes exactly that letter.
- [ ] If practical, temporarily occupy that expected letter with another valid drive/mapping and mount again; confirm DriveForge advances to the next free letter instead of failing or stealing an existing mount.
- [ ] Confirm A: and B: remain excluded from automatic selection.
- [ ] If every letter C: through Z: is occupied, confirm mounting fails cleanly instead of reusing an occupied letter.

## K. WinUI candidate smoke

The WinUI frontend remains a preview until feature/hardware parity. It must nevertheless start from the candidate package.

- [ ] Launch `WinUI\PS2-DriveForge-WinUI.exe` on the target Windows machine.
- [ ] Confirm there is no missing Windows App SDK/runtime DLL error or immediate process exit.
- [ ] Confirm `Microsoft.UI.Xaml.dll` and the Windows App Runtime payload are present beside/in the packaged WinUI deployment.
- [ ] Confirm XAML resources render and the window opens normally.
- [ ] Exercise only the functionality actually wired in this candidate; do not mark legacy parity as passed if controls are still presentation-only.

## Result

Mark one:

```text
[ ] PASS — candidate may proceed toward 0.5.0 release decision
[ ] FAIL — release blocked
```

If failed, record exact step, error text/code, relevant logs/screenshots, candidate SHA, hardware/connection and whether the failure reproduces after a clean unmount/restart. Do not modify the source HDD to investigate a read-only release failure.
