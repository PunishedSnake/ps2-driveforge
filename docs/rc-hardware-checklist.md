# Emilia 0.5 RC real-hardware checklist

Use this checklist against the **exact Windows candidate artifact** after deterministic CI is green. Record the candidate commit and artifact SHA-256 before starting.

This is a read-only validation. Any successful source mutation is release-blocking.

## Candidate identity

```text
DriveForge version / RC:
Commit SHA:
Setup/Portable SHA-256:
Windows version/build:
Windows App SDK / resolved WinUI version:
Dokany version:
PS2 HDD model/capacity:
Connection/bus/bridge:
PhysicalDrive index observed this run:
```

`PhysicalDriveN` is session-specific and may change after reboot/reconnection.

## A. Package and launcher

- [ ] Install the Setup build or unpack the Portable build to a clean directory.
- [ ] Confirm the package root is clean (`PS2-DriveForge.exe`, docs/legal files, `app`, `legacy`, `tools`, `docs`) rather than exposing WinUI runtime files at root.
- [ ] Launch root `PS2-DriveForge.exe`.
- [ ] Confirm there is no Windows loader/ordinal/missing-DLL error before the launcher runs.
- [ ] Run `PS2-DriveForge.exe --legacy` and confirm the Win32 frontend starts directly.
- [ ] Confirm `legacy\PS2-DriveForge-Win32.exe` also starts when launched directly.

## B. WinUI startup and recovery

- [ ] Start root `PS2-DriveForge.exe` normally and confirm WinUI creates/activates its window.
- [ ] Confirm the process does not die during `MainWindow::InitializeComponent()`.
- [ ] Confirm Fluent control resources render normally (NavigationView/buttons/etc.).
- [ ] Confirm `%LOCALAPPDATA%\PS2 DriveForge\Logs\winui-startup.log` contains the expected startup sequence.
- [ ] If WinUI fails, confirm the launcher offers a Win32 recovery path instead of silently exiting.
- [ ] Confirm the recovery path can open the startup log.

Known fixed RC regressions that must not return:

- incomplete self-contained WinUI runtime;
- missing `AccentFillColorDefaultBrush`;
- missing `TabViewButtonBackground` because `XamlControlsResources` was not merged;
- root launcher `COMCTL32` ordinal 345 loader failure.

## C. Win32 theme/readability

- [ ] Select **System** theme and verify normal controls/status text remain readable.
- [ ] Select **Light** theme and verify tree/list/status text contrast.
- [ ] Select **Dark** theme and verify tree/list/status text contrast.
- [ ] Specifically confirm the bottom status bar uses light text on the dark background.
- [ ] If High Contrast is available, confirm DriveForge does not override it with hard-coded dark colors.

## D. Normal-user startup and elevation

- [ ] Launch the frontend as a normal user.
- [ ] Confirm UAC behavior does not loop.
- [ ] Accept elevation for raw physical-disk access and confirm the UI remains explicitly read-only.
- [ ] Cancel elevation once and confirm image-only/non-admin behavior remains usable where supported.

## E. SetupAPI discovery and APA identification

- [ ] Confirm Windows disk interfaces are enumerated dynamically instead of presenting a fixed `PhysicalDrive0..31` list.
- [ ] Confirm PS2 HDD identification requires a valid APA scan, not model/capacity heuristics.
- [ ] If exactly one PS2 HDD candidate exists and no source is open, confirm the intended auto-open behavior.
- [ ] Record APA version, header count, main/sub counts and capacity.
- [ ] Confirm unrelated disks are not shown as PS2 HDD candidates.

Historical reference disk: APA v2, 190 headers, 43 main partitions, 147 sub-partitions. These values are evidence for that disk, not universal requirements.

## F. Partition/catalog behavior

- [ ] Confirm the base APA partition list becomes usable after one validated scan.
- [ ] Confirm sorting/filtering/selection does not rescan the source.
- [ ] Confirm optional HDL metadata enrichment does not block base-list use.
- [ ] If practical, rescan/change source during enrichment and confirm stale work does not mutate the new model.

## G. Read-only Explorer mount

- [ ] Use `Mount read-only`.
- [ ] Confirm DriveForge selects the **lowest unused letter from C: through Z:**.
- [ ] Confirm A: and B: are never selected automatically.
- [ ] Open the mount in Explorer.
- [ ] Browse the root and `Partitions`.
- [ ] Browse a known PFS partition such as `+OPL` and `__common\OPL` when present.
- [ ] Confirm normal directory opens do not produce the historical `The file exists` error.

## H. Known-file integrity

Historical validation file:

```text
__common:/OPL/conf_hdd.cfg
size:   20 bytes
SHA256: E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C
```

- [ ] On the historical disk, copy through the Explorer mount and verify the hash.
- [ ] On another disk, use a file with independently known contents/hash rather than forcing the historical value.

## I. Write rejection

Attempt operations whose only acceptable outcome is rejection:

- [ ] create file;
- [ ] create directory;
- [ ] rename item;
- [ ] delete item;
- [ ] overwrite/truncate file.

Re-read the affected source area and confirm no source metadata/content changed.

## J. Clean unmount/remount

- [ ] Use the GUI unmount action.
- [ ] Confirm the drive letter disappears.
- [ ] Confirm no orphaned Dokany state blocks a second mount.
- [ ] Mount again, browse a known file, and unmount again cleanly.

## K. Drive-letter fallback

- [ ] Record occupied drive letters before mounting.
- [ ] Predict the first free C:–Z: letter and confirm DriveForge selects it.
- [ ] If practical, occupy that letter and confirm DriveForge advances to the next free letter.
- [ ] Confirm A:/B: remain excluded.
- [ ] If all C:–Z: are occupied, confirm mount fails cleanly without stealing/reusing a letter.

## L. Rescan/source preservation

- [ ] With a source open, run rescan.
- [ ] Confirm the active source is not silently replaced.
- [ ] With multiple PS2 candidates, confirm selection is explicit rather than arbitrary.

## M. Release/legal package check

For a **public** build (not merely the private RC5 test):

- [ ] `LICENSE`, `CREDITS.md` and `THIRD_PARTY_NOTICES.md` are included.
- [ ] Dokany notices/source information match the bundled/runtime version.
- [ ] The build no longer resolves the affected Windows App SDK 2.3.1 / WinUI 2.3.0 combination.
- [ ] Windows App SDK has been upgraded to **2.4.0 or later** and the resolved WinUI package is not one of the affected versions identified by Microsoft.
- [ ] WinUI/package/startup tests were repeated after that dependency bump.
- [ ] No dependency is presented as being covered by DriveForge's MIT license when its own terms apply.

## Result

```text
[ ] PASS — candidate may proceed toward 0.5.0 release decision
[ ] FAIL — release blocked
```

For a failure, record the exact step, error text/code, relevant logs/screenshots, candidate SHA, hardware/connection and whether it reproduces after a clean restart/unmount. Do not modify the source HDD merely to debug a read-only candidate.
