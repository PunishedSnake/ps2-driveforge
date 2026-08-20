# APA implementation notes

This file documents the APA behavior DriveForge currently relies on. It is a developer aid, not a complete replacement for PS2SDK `libapa` or other primary format references.

## On-disk basics

DriveForge currently assumes the standard PS2 HDD APA layout:

- physical sector size: 512 bytes;
- APA header size: 1024 bytes (two sectors);
- little-endian on-disk fields;
- MBR/header at LBA 0;
- APA magic `0x00415041`;
- Sony MBR signature `Sony Computer Entertainment Inc.`;
- partition IDs up to 32 bytes;
- up to 64 recorded sub-partitions.

The current implementation supports little-endian hosts only and enforces this with `static_assert`.

## Header checksum

APA checksum behavior mirrors PS2SDK: treat the 1024-byte header as 256 little-endian 32-bit words, skip word 0 (the stored checksum), and sum words 1..255 with 32-bit wraparound.

DriveForge intentionally loads checksum words from bytes rather than casting the packed header to a `uint32_t*`. This avoids depending on alignment behavior of packed structures.

## Partition chain

APA main headers form a linked list through `next`/`prev` LBAs.

The reader starts at LBA 0 and validates every visited header before following the next link.

Checks currently include:

- device large enough for a header;
- valid magic;
- checksum;
- Sony MBR signature at LBA 0;
- cycle detection;
- `next` pointer within device bounds;
- `header.start` matching physical header location (warning);
- `prev` matching the previously visited header (warning);
- `nsub` clamped to the 64-entry on-disk limit with a warning.

The scan has a caller-visible maximum-header limit as a final guard against pathological metadata.

## Main and sub-partitions

APA can describe one logical partition using one main extent plus sub-partitions.

DriveForge stores the main extent and its sub descriptors in one `Partition`, then `ApaVolume` exposes them as:

```text
extent 0 = main partition
extent 1 = first APA sub-partition
extent 2 = second APA sub-partition
...
```

This matches the PFS `BlockInfo.subpart` model and prevents PFS code from caring where those extents physically live.

Do not assume sub-partitions are contiguous with their main partition or with each other. Real hardware validation already observed non-contiguous APA layouts in HDL game partitions.

## Logical versus physical addressing

`ApaVolume::read_sectors(sub, sector, count, out)` expects:

- `sub`: logical APA extent index;
- `sector`: sector offset relative to that extent;
- `count`: number of 512-byte sectors;
- `out.size() == count * 512`.

Only `ApaVolume` adds the physical `start_lba` of the selected extent.

A caller must never pre-add the main or sub-partition start LBA before passing an offset to `ApaVolume`, or the address will be translated twice.

## Partition types currently recognized

DriveForge currently names:

- FREE;
- MBR;
- EXT2SWAP;
- EXT2;
- REISER;
- PFS;
- CFS;
- HDL.

Unknown types remain visible as `UNKNOWN` rather than being rejected solely for having an unfamiliar type code.

## Safety model

APA parsing is read-only. The current `BlockDevice` API exposes no write operation.

Future APA mutation must not simply add `write()` to every existing reader path. Before any physical-disk write capability is merged, the project requires:

1. explicit writable device capability separate from the reader interface;
2. automatic backup of MBR/APA metadata involved in the operation;
3. full target-extent validation;
4. failure/recovery strategy for interrupted metadata updates;
5. image-based destructive tests before physical-HDD tests;
6. an explicit user opt-in to write mode.

## Useful hardware regression case

The first physical validation disk is approximately 149.05 GiB and contains:

- normal system PFS partitions;
- `+OPL` PFS;
- many HDL main/sub-partition layouts;
- at least one observed HDL logical layout with a sub-partition physically far away from the main partition.

That disk is useful for catching accidental assumptions that logical APA extents must be physically adjacent.
