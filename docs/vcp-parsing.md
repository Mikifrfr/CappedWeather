# Volume Coverage Pattern parsing (NEXRAD Message 31)

Question: where does a Level II volume carry its Volume Coverage Pattern, and
can the console print it without ever printing a wrong one?

Recorded here because the SCAN age block used to render a bare `VCP` token with
no value (`3444f65` dropped the token), and the next attempt at reading the
field must not be another guess. Everything below is measured against the real
volumes under `data/`.

## The field

Message 31 radial messages carry a list of data blocks (tag + `u16` size) pointed
at by the block pointers at data-header offset 32. The first is the **volume data
constant block**, tag `RVOL`, ICD 2620001 **Table XVII-E** (page 3-92), laid out
as:

| block offset | field |
|---|---|
| 0 | block type `R` + data name `VOL` |
| 4 | block size (`u16`, big-endian) |
| 6 | version major |
| 7 | version minor |
| 8 | latitude (`f32`) |
| 12 | longitude (`f32`) |
| 16 | site height (`i16`, m) |
| 18 | feedhorn height (`u16`, m) |
| 20 | reflectivity calibration (`f32`, dB) |
| 24 | transmitter power H (`f32`) |
| 28 | transmitter power V (`f32`) |
| 32 | differential reflectivity calibration (`f32`) |
| 36 | initial differential phase (`f32`, deg) |
| **40** | **Volume Coverage Pattern (`u16`, big-endian)** |
| 42 | spare |

Two block versions occur in the corpus: v1.0 is 44 bytes (2013, and KVWX 2021),
v3.0 is 52 bytes (KPAH 2021, KTLX 2026). The pattern stays at byte 40 in both;
v3.0 appends more constants after it. The coordinates at offsets 8/12 read back
as the real sites (KTLX 35.333, -97.278; KPAH 37.068, -88.772), which is the
independent check that the field alignment is right before trusting byte 40.

Cross-checked against two independent implementations of the same table:
`pyart.io.nexrad_level2.VOLUME_DATA_BLOCK` and
`xradar.io.backends.nexrad_level2.VOLUME_DATA_BLOCK` both name byte 40 `vcp`.
MetPy (`metpy.io.Level2File`, run on the same real files) decodes:

- KTLX 2013-05-20 20:12:29Z -> `VolConsts(..., phidp0=25.0, vcp=12)`
- KPAH 2021-12-11 01:03:21Z -> `vcp=212`
- KTLX 2026-10-09 16:37:06Z -> `vcp=35`

MetPy's first-radial header for the KTLX 2013 volume reads `az_angle=245.2533`,
`el_angle=0.6757`, `rad_length=6856`, `num_data_blks=7` — the same radial, byte
for byte, that is committed as a fixture below.

## What the real volumes say

`VCP` read from byte 40 of the first radial, against the elevations the volume
actually scanned (modal elevation per elevation number):

| volume | station | RVOL size | VCP | scanned tilts |
|---|---|---|---|---|
| 2013-05-20 20:12:29 | KTLX | 44 | **12** | 0.5, 0.9, 1.4, 1.8, 2.5, 3.2, 4.0, 5.1, 6.5, 8.0, 10.1, 12.5, 15.7, 19.6 |
| 2013-05-20 20:20:58 | KTLX | 44 | **12** | (same volume day) |
| 2013-05-20 20:13:33 | KINX | 44 | **12** | (same volume day) |
| 2013-05-20 20:21:24 | KINX | 44 | **12** | |
| 2021-12-11 01:03:21 | KPAH | 52 | **212** | 0.5, 0.9, 1.3, 1.8, 2.4, 3.1, 4.0, 5.1, 6.4, 8.0, 10.0, 12.4, 15.6, 19.5 |
| 2021-12-11 01:04:52 | KVWX | 44 | **212** | |
| 2021-12-11 02:23:47 | KPAH | 52 | **212** | |
| 2021-12-11 02:41:01 | KPAH | 52 | **212** | |
| 2026-10-09 16:37:06 | KTLX | 52 | **35** | 0.5, 0.9, 1.3, 1.7, 2.4, 3.1, 4.0, 5.1, 6.4 |
| 2026-10-09 16:44:10 | KTLX | 52 | **35** | |
| 2026-10-09 16:50:58 | KTLX | 52 | **35** | |

The published pattern tables agree with the scans, which is the part that would
catch a misread offset:

- VCP 12 (14 tilts): 0.5, 0.9, 1.3, 1.8, 2.4, 3.1, 4.0, 5.1, 6.4, 8.0, 10.0,
  12.5, 15.6, 19.5 — the 2013 KTLX/KINX volumes scan exactly this ladder (the
  sub-0.2 deg differences are antenna pointing, not different cuts).
- VCP 212 (14 tilts, recombined): same ladder as VCP 12 — matches KPAH/KVWX.
  VCP 215 would instead show 12.0, 14.0, 16.7, 19.5; those elevations do not
  appear, so 215 is excluded for these volumes.
- VCP 35 (9 tilts, clear air): 0.5, 0.9, 1.3, 1.8, 2.4, 3.1, 4.0, 5.1, 6.4 —
  matches the 2026 KTLX volumes exactly, including the fact that they stop at
  6.4 deg.

Split cuts also line up: VCP 12 repeats its lowest three elevations, so the 2013
KTLX volume has 17 elevation numbers for 14 tilts; VCP 35 repeats its first
three (12 elevation numbers for 9 tilts); KPAH 2021 has 21 (14 tilts plus SAILS
and split-cut repeats). The pattern number is the only field that explains all
three volumes' structures at once.

The same numbers come out of the app's own decode path
(`decodeLevel2Archive`, bzip record unpack plus merge) via a throwaway probe:

| volume | records | messages | rejectedBlocks | decoded VCP |
|---|---|---|---|---|
| KTLX 2013-05-20 20:12:29Z | 0 (raw) | 8,280 | 0 (was 24,840) | 12 |
| KPAH 2021-12-11 01:03:21Z | 94 (bzip) | 11,160 | 0 | 212 |
| KVWX 2021-12-11 01:04:52Z | 82 (bzip) | 9,720 | 0 | 212 |
| KTLX 2026-10-09 16:37:06Z | 55 (bzip) | 6,480 | 0 | 35 |

## What the field is not

- block byte 18 (`u16` 34 for KPAH, 19 for KTLX) is feedhorn height in metres,
  not a pattern.
- block byte 36 (`f32` 25.0 / 60.0) is initial differential phase.
- the spare halfword at byte 42 is 0 in the 2013 corpus and 3 in 2021+ volumes.

## Implementation and fail-closed rules

`src/core/level2_decoder.cpp` reads the pattern from a well-formed `RVOL` block
(tag match, declared size >= 44 and inside the radial) and accepts it only when
`1 <= pattern <= 999`. A radial whose block is missing, short, declares pattern
0 (maintenance) or an out-of-range value reports no pattern. Radials of one
volume that disagree set `volumeCoveragePatternConflicting` and the pattern is
reported as 0: a volume that spans a pattern change does not get one number.
`mergeVolumeCoveragePattern` folds record-by-record (archive) or chunk-by-chunk
(live) decoding with the same rule, and `DecodedChunk::volumeCoveragePattern`
(0 = not carried) survives the spill format (v2; v1 spill files still load with
pattern 0).

`RVOL`, `RELV`, `RRAD` and `DCFP` are constant/auxiliary blocks, not moment data,
so a clean radial no longer counts them as `rejectedBlocks` (before this change
every radial added three or four phantom rejections to the parser-rejected
badge: 24,840 for one 2013 KTLX volume, 3 per radial for the 2013 layout and 4
where the clutter-filter power block is present).

## Fixtures

Three radials are committed verbatim from the archive volumes above — the exact
bytes of one Message 31 message, starting at the ICAO:

- `tests/fixtures/level2_radial_ktlx_20130520_vcp12.bin` (6,856 B, VCP 12)
- `tests/fixtures/level2_radial_kpah_20211211_vcp212.bin` (9,928 B, VCP 212)
- `tests/fixtures/level2_radial_ktlx_20261009_vcp35.bin` (9,928 B, VCP 35)

`tests/core_tests.cpp::testLevel2VolumeCoveragePattern` decodes them and asserts
the pattern, the ray geometry and that no constant block is counted as rejected.
`testLevel2VolumeCoveragePatternSyntheticBytesOnly` covers the fail-closed paths
with the minimal byte layout the spec requires, and its name says so.
