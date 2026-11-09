# HDLoader format notes

These notes describe what DriveForge implements for HDLoader (`0x1337`) game partitions. They were cross-checked against hdl-dump (`hdl.c`, `iin_hio.c`) and Open PS2 Loader (`modules/iopcore/cdvdman`), but they are not a replacement for those primary sources.

## Partition layout

An installed game owns one APA main partition plus up to 64 sub-partitions.

| Region | Contents |
|---|---|
| main `+0x000000` .. `+0x400000` | APA header, OSD icon data, `0xDEADFEED` header at `+0x101000`, optional KELF at `+0x111000` |
| main `+0x400000` .. end | game data |
| each sub `+0x000000` .. `+0x100000` | APA sub header / reserved |
| each sub `+0x100000` .. end | game data |

DriveForge treats those reserved windows as off-limits for game data. A slice pointing into them is rejected.

## The `0xDEADFEED` header

| Offset | Size | Field |
|---|---|---|
| `0x000` | u32 | magic `0xDEADFEED` |
| `0x008` | char[] | title (NUL-terminated, ≤ 64 chars) |
| `0x0A9` | u8 | compatibility flags |
| `0x0AA` | u16 | DMA mode |
| `0x0AC` | char[13] | startup ELF (`SLUS_123.45`) |
| `0x0E8` | u32 | DVD9 layer break |
| `0x0EC` | u32 | media: `0x14` DVD, `0x12` CD |
| `0x0F0` | u32 | number of allocation slices (1..65) |
| `0x0F4` | 12 × n | allocation table (below) |

## The allocation table trap

hdl-dump writes **and** reads the table through a `u32*` at `+0x0F5`, one byte past the natural position, with shifted units:

```text
hdl-dump view @ +0x0F5:  { offset in 512 KiB units, start_sector >> 8, size_kb * 4 }
```

Its own source calls this "aerial acrobatics". The code that actually boots the game (HDLoader, OPL `cdvdman`) reads aligned records at `+0x0F4`:

```text
loader view @ +0x0F4:    { part_offset  : ISO offset in 2048-byte sectors,
                           data_start   : absolute HDD sector (512 bytes),
                           part_size    : slice size in bytes }

hdd_lba = data_start + (iso_lsn - part_offset) * 4
```

Because hdl-dump's values are whole multiples of 256 in the shifted fields, both views decode to the same numbers on hdl-dump-written disks. That also explains why 65 records at `+0x0F4` end exactly at byte 1024 while the `+0x0F5` view overruns by one byte (the "historical one-byte boundary overrun" noted in `hdl.hpp`).

DriveForge decodes the **loader view**, because it defines what the console reads. The legacy `raw_size_bytes` value from the `+0x0F5` view is kept for compatibility and is regression-tested to agree.

## Virtual ISO validation (Frieren)

`hdl::read_game_layout()` fails closed unless every slice:

1. has a non-zero size that is a multiple of 2048 bytes;
2. starts exactly where the previous slice ended in ISO space;
3. lies entirely inside the data area of this game's own main or sub extents;
4. stays inside the backing device;
5. does not overlap any other slice on disk.

The ISO size is the sum of slice sizes. Export also checks for the ISO9660 primary volume descriptor (`CD001` at sector 16) and emits a warning, not an error, when it is missing.

hdl-dump's legacy second-slice addressing (`SLICE_2_OFFS`, an extra 0x10000000-sector bias for a second APA table on very old >128 GiB setups) is not interpreted. Such slices would fail validation rather than be read from the wrong place.
