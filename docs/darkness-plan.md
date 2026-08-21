# Darkness 0.4 development plan

Darkness adds a read-only Windows filesystem provider on top of the hardware-validated Chisato stack.

## Definition of done

A supported PS2 HDD image or physical `PhysicalDriveN` can be mounted through Dokany and browsed in Windows Explorer. PFS files copied through Explorer must be byte-identical to DriveForge's direct reader/export path. Every source-device operation remains read-only.

## Namespace

Initial 0.4 namespace:

```text
<mount>\
  Partitions\
    <PFS partition>\
      <PFS tree>
```

`Games` and synthetic HDL ISO views remain 0.7 Guts work. MBR/recovery synthetic files remain later work.

## Implementation order

1. portable `ReadOnlyMountView` for lookup/list/read path mapping;
2. thread-safe `DriveSession::stat` and `DriveSession::read_file` operations;
3. deterministic Windows-safe aliases for PFS/APA path components;
4. Dokany 2.x adapter (`ZwCreateFile`, `ReadFile`, `GetFileInformation`, `FindFiles`, volume/free-space callbacks);
5. hard rejection of all mutating callbacks and `DOKAN_OPTION_WRITE_PROTECT`;
6. CLI mount frontend for image and `PhysicalDriveN` sources;
7. Windows CI compile/package with Dokany SDK;
8. generated-image mounted-read tests where CI permits, otherwise callback-independent mount-view tests plus manual Windows mount smoke test;
9. real-HDD Explorer validation and SHA-256 comparison.

## Safety invariants

- no `BlockDevice::write()` is introduced;
- `PhysicalDrive` continues to request `GENERIC_READ` only;
- Dokany is an adapter above `ReadOnlyMountView`/`DriveSession`, never a parser layer;
- create/write/delete/rename/truncate/attribute mutation operations return write-protected/access-denied status;
- source-device handles never become writable as a side effect of mounting.

## Performance boundary

Darkness targets filesystem correctness and stable Explorer behavior. Major caching, read-ahead, request coalescing and overlapped physical I/O remain 0.5 Emilia. Instrumentation should remain enabled so Darkness workloads produce the baseline Emilia will optimize.

## Hardware validation target

On the current real test HDD:

1. mount the PS2 HDD read-only;
2. browse `Partitions\+OPL` in Explorer;
3. browse `Partitions\__common\OPL`;
4. copy `conf_hdd.cfg` through Explorer;
5. verify SHA-256 `E94F190BA999E6621B55C290AD494CFF6421F08C470E9424AED7B2A4B085890C`;
6. recursively copy `+OPL`;
7. verify file creation, rename, deletion and writes are rejected;
8. unmount cleanly;
9. record backing-I/O statistics for the mounted workload.

Generated-image tests continue to cover SEGI and APA main/sub-partition crossings that are not available as real PFS content on the current HDD.
