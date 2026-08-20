# PFS implementation notes

This is a developer-facing map of the PFS details DriveForge currently depends on. It is intentionally narrower than a complete PFS specification: every item here should correspond to code we have implemented, validated, or need to preserve when refactoring.

Primary references are PS2SDK `libpfs`/`libapa` behavior and pfsshell compatibility code. Do not treat this file as an independent specification when the implementation and upstream format references disagree.

## Units and address spaces

The easiest PFS bug to introduce is confusing four different units:

```text
host byte offset
    -> 512-byte PS2 HDD sector
        -> PFS zone
            -> PFS BlockInfo.number
```

Important rules used by DriveForge:

- APA/PFS disk sectors are 512 bytes.
- PFS metadata records are 1024 bytes.
- A normal PFS zone is a power of two between 2 KiB and 128 KiB.
- `BlockInfo.number` is a **zone number**, not a sector number.
- `BlockInfo.subpart` selects APA extent 0 (main) or one of the PFS sub-partitions.
- file data described by a `BlockInfo` begins at `number * zone_size` within that APA extent.

`ApaVolume` is the boundary that converts `(subpart, sector)` into physical disk LBA. PFS code should not add APA main/sub start LBAs itself.

## Superblock

DriveForge currently probes:

- primary superblock at partition-relative sector 8192;
- backup superblock at sector 8193;
- magic `0x50465300`;
- format versions up to v3;
- zone-size validity;
- number of PFS sub-partitions;
- root and journal sub-part references;
- primary/backup equality;
- fsck status warnings.

A primary/backup mismatch is a warning during read-only inspection, not permission to "repair" anything.

## Metadata block addressing

PFS metadata structures are 1024 bytes, while the filesystem's allocation unit is the zone. PS2SDK addresses inode metadata by shifting the zone-number-like `BlockInfo.number` according to the ratio between 1024 bytes and the current zone size.

DriveForge expresses that as `Reader::inode_scale()` and then converts the resulting metadata block to 512-byte sectors.

Do not simplify inode loading to:

```text
sector = BlockInfo.number
```

or:

```text
sector = BlockInfo.number * sectors_per_zone
```

Both are wrong for metadata.

## SEGD inode layout

The normal inode metadata record has `SEGD` magic and is exactly 1024 bytes.

The important descriptor rule is:

- `data[0]` describes the metadata segment itself;
- file/directory payload begins with `data[1]`;
- the primary SEGD contains the direct data descriptors;
- `number_data` is treated as the global descriptor count, including metadata descriptors required by the format.

DriveForge validates the 1024-byte metadata checksum before using descriptor contents.

## SEGI indirect descriptors

Large or heavily fragmented files can continue through `SEGI` records.

A subtle format detail is that an indirect SEGI metadata record can use the bytes that serve other inode metadata purposes in a SEGD as additional `BlockInfo` entries. This produces 123 descriptor slots in the indirect record.

DriveForge therefore treats the chain conceptually as:

```text
SEGD
  data[0]      = self
  data[1..]    = payload extents
  next_segment -> SEGI

SEGI
  data[0]      = self
  data[1..122] = payload extents
  next_segment -> next SEGI
```

The exact index mapping is tested by `pfs_segi_tests.cpp`. Refactors must preserve that test before touching the traversal algorithm.

## Directory data

A PFS directory is read through the same logical byte-stream machinery as a regular file, but its payload contains variable-size directory entries.

The current parser enforces the important 512-byte boundary rule: a directory entry must not straddle a sector boundary. This means parsing should not be replaced by an ordinary packed-struct walk over a large arbitrary buffer without retaining the boundary checks.

Directory names are PFS names. Windows filename policy does **not** belong here; conversion to legal Windows host names is handled by `ps2driveforge_host`.

## Byte-range reads

Windows/Dokany callers may request offsets that are not aligned to a PS2 sector or PFS zone.

`Reader::read()` therefore exposes a logical byte-range operation. `read_zone_bytes()` currently:

- uses a scratch 512-byte sector for unaligned edges;
- batches aligned middle portions;
- caps each aligned lower-level batch at 128 sectors (64 KiB today).

The 128-sector value is an implementation choice, not a PFS format property. It should eventually become part of the measured I/O strategy.

## Error philosophy

The read path is deliberately suspicious of metadata. It rejects or reports:

- invalid magic;
- checksum failures;
- missing sub-partitions;
- overflowing address calculations;
- null/invalid SEGI pointers;
- empty descriptors inside the used range;
- a descriptor stream too short for the requested file range;
- malformed directory entries.

Do not weaken these checks merely to make one damaged disk "work". If compatibility requires tolerant behavior, add an explicit diagnostic/recovery mode so normal reads remain strict.

## Hardware observations so far

The real test HDD used during Ayanami/Bocchi validation has:

- APA v2;
- PFS v3 system/user partitions;
- 8 KiB PFS zones;
- matching primary/backup superblocks on observed PFS partitions;
- `+OPL` root entries `CFG`, `THM`, `LNG`, `ART`, `VMC`, `CHT`, and `APPS`;
- an empty `+OPL:/CFG` directory at validation time.

See `REAL_HARDWARE_VALIDATION.md` for the separation between synthetic-test coverage and actual physical-disk observations.
