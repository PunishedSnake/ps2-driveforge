# Frieren OPL Asset Pipeline

Frieren treats game installation as more than copying an ISO into HDL extents. After DriveForge extracts the PS2 startup ID from `SYSTEM.CNF`, the same identity can drive optional title metadata, compatibility settings, widescreen CHT, mastercode verification, artwork and HDD-OSD lookup.

The pipeline is deliberately split into **planning**, **host fetching/staging** and **destination mutation**. Downloading bytes is not permission to write them to PFS. The Internet has plenty of confidence already.

## Identity

Per-game providers use canonical OPL serial form, for example:

```text
SLUS_209.46
```

`canonical_game_id()` accepts common spellings such as `SLUS-20946`, `SLUS20946` and `cdrom0:\\SLUS_209.46;1`, but refuses values that require guessing.

For new installs, ISO `SYSTEM.CNF` remains authoritative. An online title database may improve presentation, but it does not replace the startup identity read from the game.

## Providers

| Data | Primary provider | Fallback / overlay | Destination intent |
| --- | --- | --- | --- |
| Game title catalog | `israpps/HDL-Batch-installer/Database/gamename.csv` | ISO filename/title | DriveForge cache |
| Redump catalog | `israpps/HDL-Batch-installer/Database/redump.csv` | none | DriveForge cache |
| OPL CFG metadata | `israpps/PS2-OPL-CFG-Database/CFG_en/<ID>.cfg` | none | `CFG/<ID>.cfg` |
| HDD compatibility CFG | `GDX-X/PS2-OPL-CFG-Compatibility-Database/HDD/<ID>.cfg` | overlay | `CFG/<ID>.cfg` |
| Widescreen CHT | `PS2-Widescreen/OPL-Widescreen-Cheats/CHT/<ID>.cht` | none | `CHT/<ID>.cht` |
| Verified mastercode | embedded in primary CHT | `PS2-Widescreen/Bare-Mastercodes-bin/MASTERCODES/<ID>.cht` | same CHT |
| OPL artwork | `Luden02/psx-ps2-opl-art-database/PS2/<ID>/...` | Archive.org OPLM artwork snapshot | `ART/` |
| HDD-OSD icon | `CosmicScale/HDD-OSD-Icon-Database/ico/<ID>.ico` | none | HDD-OSD domain, not `+OPL` ART |

Provider URLs are configuration/data, not filesystem architecture. The plan records provider, source and destination intent so replacing one provider does not require teaching PFS about GitHub.

## CFG merge policy

DriveForge does not blindly replace a useful CFG with a smaller compatibility fragment.

Host-side provider merge uses:

1. normal metadata CFG as base;
2. HDD compatibility CFG as overlay;
3. matching `key=value` entries replaced by the overlay;
4. new overlay keys appended;
5. unrelated base comments/metadata preserved.

During PFS import, existing user-local CFG content is also treated as input where applicable. User DMA/VMC/compatibility choices are not disposable simply because a network database had a productive morning.

The preview/import result distinguishes create, unchanged, merge and replacement behavior rather than hiding all four behind one green "installed" state.

## CHT and mastercodes

The widescreen CHT is primary.

- If it already contains a `Mastercode` section, no fallback is appended.
- Otherwise DriveForge may fetch the matching bare mastercode.
- Only the fallback `Mastercode` section is appended, not a duplicate title/header.
- If no verified mastercode exists, the widescreen file may still be staged with an explicit warning/state.

The UI should preserve the distinction between widescreen data found and mastercode verified. Those are related facts, not one checkbox with excellent self-esteem.

## Artwork policy

Default game install requests cover, icon, first background and first screenshot. Logo/disc label are optional expanded-artwork requests.

Provider results preserve the actual source extension. A PNG does not become a JPEG by renaming it because one older tool expected a particular suffix.

Artwork is optional game presentation data. Missing artwork should not turn an otherwise valid HDL install into storage failure unless the caller explicitly marks it required.

## Classic and TAR layouts

### Classic loose files

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

Fetching always stages logical assets first. TAR parse/update/serialization belongs to the destination layer. A failed HTTP request therefore cannot directly mutate an existing on-disk archive.

Frieren now has a real TAR destination path. Members are merged into the archive container and the resulting archive is written through the same native PFS writer and normal-reader verification as other files.

## Network and cache boundary

Windows uses WinHTTP with normal TLS certificate validation, finite timeouts and an explicit maximum response size. There is no certificate-bypass mode.

Portable tests inject a fake `HttpClient`, so Linux CI does not depend on provider availability or the mood of Archive.org.

Catalogs may be cached until refresh. Per-game assets are staged below controlled host paths. Empty successful responses, apparent HTML error bodies and unsafe/rooted/parent-traversing destination paths are rejected.

Fetched results preserve provider/source provenance and whether data came from cache, merge or verified-mastercode fallback.

## OPL PFS partition resolution

DriveForge does not assume the target is always a partition literally named `+OPL`.

Resolution policy is:

1. require a clean APA scan;
2. if the user configured a partition, require that exact main partition to be PFS and valid;
3. otherwise consider main PFS partitions only;
4. probe candidates through normal PFS rules;
5. score strong name/evidence such as `+OPL`, `ART`, `CFG`, `CHT`, `VMC` and secondary OPL directories;
6. refuse a top-score tie rather than choosing the first partition returned by a vector.

Ambiguity is a UI state, not permission to improvise.

## Native PFS import

The Asset Pipeline now reaches the image-only PFS destination layer.

Conceptually:

```text
staged provider assets
 -> destination preparation
 -> resolve OPL PFS partition
 -> normalize and deduplicate paths
 -> read existing destination state
 -> merge local CFG where applicable
 -> create required directories
 -> loose-file or TAR update plan
 -> native PFS batch write
 -> flush/readback
 -> normal PFS reader verification
```

Cache-only data is omitted from PFS. HDD-OSD icon placement stays a separate destination domain.

The importer freezes host-side bytes before mutation so the file being written cannot change underneath the approved plan. The filesystem side then uses native PFS allocation/publication rules documented in [`frieren-pfs-write.md`](frieren-pfs-write.md).

## Combined game deployment

`image_game_deploy` coordinates HDL game installation and optional OPL asset import for writable images.

Important ordering/state rules:

- all available preflight work is performed before mutation;
- the OPL PFS destination is resolved before HDL APA publication;
- after structural HDL publication, APA is rescanned and the selected PFS partition is re-identified by expected start LBA, ID and type rather than reusing a stale object;
- PFS asset import follows only after the HDL game is valid;
- final APA and HDL verification runs again after PFS mutations.

A failure during the asset phase can therefore yield a valid installed game with a partial asset result. The result reports that state explicitly instead of pretending multi-domain deployment is one atomic filesystem transaction.

## Licensing and redistribution boundary

Provider use does not imply DriveForge may bundle an entire upstream database. Runtime fetching is preferred where redistribution rights are unclear or content belongs to game/art owners.

See [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) for current provider/licensing notes.

## Physical disk boundary

All PFS asset mutation described here remains image-only in Frieren development. The Windows `PhysicalDrive` backend stays read-only.

A future physical writer must separately prove source identity, Windows locking/cache behavior, flush/readback, interruption recovery and real PS2 consumption. A successful WinHTTP request is, perhaps unsurprisingly, not one of those gates.
