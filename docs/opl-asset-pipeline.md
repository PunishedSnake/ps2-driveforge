# Frieren OPL Asset Pipeline

Frieren treats game installation as more than copying an ISO into HDL extents. After DriveForge identifies the PS2 startup ID from `SYSTEM.CNF`, the same identity can drive optional metadata, artwork, compatibility and cheat/fix lookups.

The asset layer is intentionally split into **planning**, **host-side fetching/staging**, and a later **PFS import transaction**. Downloading a file must never be enough to mutate the PS2 disk.

## Identity

All per-game providers use the canonical OPL serial form, for example:

```text
SLUS_209.46
```

`canonical_game_id()` accepts common spellings such as `SLUS-20946`, `SLUS20946`, and a `cdrom0:\\SLUS_209.46;1` path, but refuses input that cannot be normalized without guessing.

The ISO parser remains authoritative for a new install. An online title database may improve the displayed title, but it does not replace `SYSTEM.CNF` as the game identity source.

## Providers

| Data | Primary provider | Fallback / overlay | Destination intent |
| --- | --- | --- | --- |
| Game title catalog | `israpps/HDL-Batch-installer/Database/gamename.csv` | ISO filename/title remains usable | DriveForge cache |
| Redump catalog | `israpps/HDL-Batch-installer/Database/redump.csv` | none | DriveForge cache |
| OPL CFG metadata | `israpps/PS2-OPL-CFG-Database/CFG_en/<ID>.cfg` | none | `CFG/<ID>.cfg` |
| HDD compatibility CFG | `GDX-X/PS2-OPL-CFG-Compatibility-Database/HDD/<ID>.cfg` | merged over metadata CFG | `CFG/<ID>.cfg` |
| Widescreen CHT | `PS2-Widescreen/OPL-Widescreen-Cheats/CHT/<ID>.cht` | none | `CHT/<ID>.cht` |
| Verified mastercode | embedded mastercode in widescreen CHT | `PS2-Widescreen/Bare-Mastercodes-bin/MASTERCODES/<ID>.cht` | merged into the same CHT |
| OPL artwork | `Luden02/psx-ps2-opl-art-database/PS2/<ID>/...` PNG | OPL Manager `OPLM_ART_2023_07` Archive.org mirror | `ART/` |
| HDD-OSD icon | `CosmicScale/HDD-OSD-Icon-Database/ico/<ID>.ico` | none | HDD-OSD metadata path, not `+OPL` |

Provider URLs are data, not architecture. The planner records the provider and target intent so a future provider can replace an old one without touching HDL/PFS code.

## CFG merge policy

DriveForge does not blindly replace a useful CFG with a smaller compatibility fragment.

When both sources exist:

1. the normal CFG metadata file is the base;
2. the HDD compatibility file is treated as an overlay;
3. matching `key=value` entries from the compatibility file replace the base value;
4. new overlay keys are appended;
5. unrelated base metadata and comments remain intact.

When PFS import is implemented, an already-existing user CFG must become a third input with an explicit preview. User-local settings are not disposable merely because an HTTP request succeeded.

## CHT and mastercodes

The widescreen CHT is always the primary file. DriveForge parses it first.

- If it already contains a `Mastercode` section, no fallback is fetched or appended.
- If it does not contain a mastercode, DriveForge may fetch the matching file from `Bare-Mastercodes-bin`.
- Only the fallback `Mastercode` section is appended. The fallback title/header is not duplicated.
- If no verified mastercode is available, the widescreen file can still be staged, but the result carries a warning instead of inventing a code.

Future UI should expose this distinction as `widescreen found`, `mastercode verified`, or `mastercode unavailable` rather than one misleading green checkbox.

## Artwork policy

The current primary source is the static PNG-oriented OPL Manager artwork dump arranged by game ID. The older Archive.org OPLM ART snapshot is a fallback.

Default game install requests:

- cover;
- icon;
- first background;
- first screenshot.

Logo and disc label are optional `all artwork` extras. The provider plan preserves the actual extension of the selected source, so a PNG primary does not become a `.jpg` file merely because an older downloader expected one.

## Classic and TAR layouts

The planner supports two destination layouts without changing providers:

### Classic OPL

```text
ART/<ID>_COV.png
ART/<ID>_ICO.png
ART/<ID>_BG_00.png
ART/<ID>_SCR_00.png
CFG/<ID>.cfg
CHT/<ID>.cht
```

### TAR-capable OPL forks

```text
ART/art.tar :: <ID>_COV.png
CFG/cfg.tar :: <ID>.cfg
CHT/cht.tar :: <ID>.cht
```

The fetcher stages TAR members as individual verified files. Actual TAR mutation/packing belongs to the destination layer so an interrupted network request can never corrupt an existing on-disk archive.

## Network and cache boundary

On Windows the native host layer uses WinHTTP with normal certificate validation, finite timeouts and an explicit per-download byte limit. There is no `--no-check-certificate` equivalent.

Portable regression tests inject a fake `HttpClient`, so Linux sanitizer CI does not depend on GitHub or Archive.org availability.

Catalog files are cached and reused unless refresh is requested. Per-game assets are staged below a game-ID directory before any PS2 storage mutation. HTML error pages and empty successful responses are rejected instead of being written as fake `.cfg`, `.cht` or artwork files.

## PFS write boundary

As of the first Asset Pipeline slice, DriveForge can:

- derive an asset plan from the ISO startup ID;
- fetch and merge provider data;
- stage the exact files and record their intended OPL destination;
- describe classic and TAR destinations.

It **cannot yet write those files into the OPL PFS partition**. The current PFS implementation is intentionally read-only.

The next write milestone is an image-only PFS mutation layer with a pure plan/apply split, before-images for filesystem metadata, cold reopen verification through the existing PFS reader, and explicit OPL partition resolution rather than a hard-coded `+OPL` assumption.

Only after that layer is proven on disposable images may downloaded assets be committed to real PS2 storage.
