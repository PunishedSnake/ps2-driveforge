# PFS read path

PS2 DriveForge reads PFS directly from an `ApaVolume`; it does not emulate iomanX.

The read path follows the on-disk structures used by PS2SDK:

1. PFS superblock and backup superblock
2. root SEGD inode
3. direct `BlockInfo` data extents
4. chained SEGI descriptors when required
5. 512-byte directory-entry sectors
6. inode resolution for files and directories
7. byte-range reads across PFS extents and APA sub-partitions

## Safety

The current API is read-only. No PFS or APA write primitive is exposed.

## Segment descriptor layout

A primary SEGD inode contains 114 `BlockInfo` slots and filesystem metadata in the tail of the 1024-byte inode. For indirect SEGI records PS2SDK uses the entire area after the fixed 40-byte prefix as 123 `BlockInfo` records. Local slot 0 identifies the descriptor itself and subsequent slots describe file data.

This distinction matters for fragmented or very large files and is handled explicitly by the DriveForge reader rather than by copying the PS2SDK cache/iomanX layer.
