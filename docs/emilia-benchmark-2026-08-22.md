# Emilia real-HDD benchmark — 2026-08-22

This note records the first complete Emilia benchmark sweep on the project's hardware-validation PS2 HDD. It is a measurement baseline, not a cross-tool speedup claim.

## Source profile

The Windows storage stack reported:

```text
media class:            unknown
seek penalty:           unknown
TRIM enabled:           no
bus type:               11 (SATA)
nominal rotation rate:  unknown
```

The device does not expose enough information for DriveForge to classify it permanently as rotational or solid-state. Observed single-read service time is nevertheless consistently around 8.2–8.7 ms, so the automatic policy correctly remains conservative instead of converting a latency observation into a hard media identity.

## APA scan and zero-I/O catalog

Across all 24 benchmark process launches in this sweep:

```text
Cold APA scan median:   1555.022 ms
Cold APA scan range:    1532.589–1584.905 ms
APA headers:            190
Backing reads / scan:   190
Backing bytes / scan:   190.00 KiB

Catalog build median:   0.006 ms
Catalog build range:    0.004–0.017 ms
Catalog rows:           190
Main / sub:             43 / 147
HDL partitions:         35
PFS partitions:         7
HDL allocated:          133.62 GiB
PFS allocated:          2.62 GiB
```

The HDD Manager contract is therefore validated on real hardware: once the single APA scan is complete, producing the complete management list is effectively free and performs no additional source I/O.

## PFS metadata cache

Three runs were recorded for each workload.

### `+OPL /`

```text
Cold browse + stat median:  41.493 ms
Cold backing reads:          10
Cold backing bytes:          16.00 KiB
Warm browse + stat median:   0.001 ms
Warm backing reads:          0
```

The cold workload records one directory-cache miss and one PFS-probe miss. The immediately repeated workload is served entirely from session metadata caches and performs zero backing reads.

### `__common /OPL`

```text
Cold browse + stat median:  27.689 ms
Cold backing reads:          6
Cold backing bytes:          15.00 KiB
Warm browse + stat median:   0.001 ms
Warm backing reads:          0
```

This also becomes a zero-I/O metadata workload on the warm pass.

For reference, the earlier Chisato `+OPL` browse baseline was 215 backing reads / 206.50 KiB. The workloads are not perfectly identical, but Emilia's real-HDD metadata path now demonstrates the intended cache behavior directly rather than relying on synthetic tests alone.

## Native HDL metadata scheduler sweep

All variants discovered 35 HDL games, read 35/35 successfully, read 52.50 KiB total, and preserved ascending physical-LBA ordering.

Median of three runs:

| Policy | Wall time | Average backing-read service | Max in flight | Relative to QD1 |
| --- | ---: | ---: | ---: | ---: |
| automatic | 304.467 ms | 8.696 ms | 1 | effectively QD1 |
| QD1 | 304.461 ms | 8.696 ms | 1 | baseline |
| QD2 | 309.132 ms | 17.009 ms | 2 | 1.5% slower |
| QD4 | 321.172 ms | 34.920 ms | 4 | 5.5% slower |
| QD8 | 275.095 ms | 56.146 ms | 8 | 9.6% faster wall time |

QD8 wins this narrow all-game completion benchmark by about 29 ms, but raises average per-read service latency by roughly 6.46x. QD2 and QD4 are both slower than QD1 while also increasing latency.

### Policy decision

Keep the normal `unknown`-media automatic policy at **QD1** for now.

Reasons:

- the complete APA management list is already available before HDL enrichment starts;
- enrichment is optional and progressive, so first-result latency and UI responsiveness matter more than shaving ~29 ms from all-game completion;
- QD1 is stable and matches the observed physical service latency;
- QD2/QD4 provide no throughput benefit on this device;
- QD8's modest completion-time gain comes with a very large latency increase and should not become a blanket policy for old PS2 HDDs or unknown bridges.

A future throughput-biased policy may revisit higher queue depths for positively identified SSDs or for devices where measurements demonstrate a sustained benefit without pathological latency. One real disk is not enough evidence for a universal QD rule.

## WinUI implication

The native WinUI frontend should reflect the measured architecture rather than display a modal loading screen:

```text
open source
  -> one APA scan (~1.55 s on this disk)
  -> publish all 190 management rows immediately
  -> first paint / filter / sort / selection are memory-only
  -> enrich 35 HDL rows progressively in the background (~0.30 s at current safe policy)
```

PFS browsing should preserve the same session so repeated directory/stat activity can benefit from the validated warm-cache behavior.

## Safety

All measurements were read-only. The physical source path remains `GENERIC_READ` only, and this benchmark does not justify weakening any existing format-validation, corruption, or write-protection gate.
