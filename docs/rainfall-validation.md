# Z-R rainfall accumulation — validation record and limits

Status: **not validated. Zero cases scored.**

The engine exists (`accumulateRainfall` in `src/core/derived_products.cpp`) and
is exercised by closed-form core tests, but it has **never been compared with a
gauge or with MRMS**. Nothing in the app presents it as a measurement, and
nothing may until the gates in "Comparison plan" below are met and logged in
"Scoring log". Until then every number it produces is a lower-bound guidance
estimate derived from reflectivity, not observed rainfall.

This file is the record that makes the absence of validation explicit. It is
required by the honesty bar: no precision claim without a scored record.

## What the engine computes

A single Z-R relation per gate on the lowest reflectivity tilt of each volume:

```text
Z  = 10^(dBZ/10)                [mm^6/m^3]
Z' = min(Z, 10^(53/10))         [hail cap]
R  = (Z' / 300)^(1/1.4)         [mm/h]   (Marshall-Palmer)
```

Defaults live in `RainfallConfig`; `A = 300`, `b = 1.4`, gate floor 5 dBZ, hail
cap 53 dBZ (103.8 mm/h). `zrRateMillimetersPerHour` is the single source of the
relation, so tests and the UI cannot drift apart.

Rain is integrated by **left hold** between volume times: the rate measured at
volume *i* is credited over `[t_i, t_i+1)`. The last volume is credited nothing
unless the caller passes a closing clock. An interval longer than 600 s means
volumes are missing, so it is credited as 600 s and every gate it touched is
flagged `GapClamped` — a multi-hour gap is never poured through one rate.

Output is per gate in **0.1 mm** (a tipping-bucket resolution, not a claim of
Z-R accuracy), plus a summary of what the number is actually made of: samples
used, window length, credited seconds, gap-clamped intervals, and per-flag gate
counts. The reference grid is the newest volume's lowest tilt, so the
accumulation lines up with the reflectivity the user is looking at; each ray
reports the reference chunk's ray index.

## What history the app can actually hand it

This is the ceiling the product lives under, measured from the code, not
estimated:

| Store | Limit | Source |
|---|---|---|
| Resident volumes | `maximumHistoryFrames_`, default **12**, clamped **2..32** | `main_window.cpp` setting `timeline/maximumFrames` |
| Spilled tail | `kSpilledKeepVolumes = 8` | `session_store.hpp` |
| Hard ceiling | **40 volumes** (32 resident + 8 spilled) | derived |
| Default ceiling | **20 volumes** | derived |

WSR-88D volume cadence is typically 4-6 minutes (about 2.5 min with SAILS cuts
in VCP 12); European OPERA cycles are slower and less regular. At 5 min/volume
the default 20 volumes is about **100 minutes** of scan history, and the
settings ceiling of 40 volumes is about **3.3 hours** before any missing volume
is counted. A "storm total" is therefore only as long as the frames the session
still holds — it is not an archive product.

`DecodedChunk` carries **no scan time**, and the spill format
(`volume_spill.cpp`, `CWDV` v1) has no time field. The timestamp lives in
`RadarFrame::dataTime` (`session_store.hpp`). A caller that faults a spilled
volume back in must pair it with its frame time; file mtime is not a substitute.
That is why `RainfallSample` takes the time from the caller instead of reading
it from the chunk.

## Guard and quality rules

The AP/clutter guard is part of the product, not a later refinement. Order of
application per gate per interval:

| Stage | Rule (default) | Effect |
|---|---|---|
| Range | ground range > 230 km | no depth, `RangeLimited` |
| Beam height | beam > 4000 m MSL | no depth, `BeamHigh` |
| Beam height | beam > 2000 m MSL | depth kept, `BeamHigh` |
| Dual-pol | CC present and < 0.80 | no depth, `NonMeteorological` |
| Vertical | Z >= 40 dBZ and column echo top < 3000 m MSL, with the volume having observed above 3000 m at that range | no depth, `ClutterSuspect` |
| Vertical, blind | Z >= 40 dBZ but no tilt at that range reached 3000 m (single-tilt volume, short range) | depth kept, `NoVerticalCheck` |
| Hail | Z > 53 dBZ | rate clamped at 53 dBZ, `HailCapped` |
| Structural gap | no ray at the azimuth, or the gate lies beyond that sample's ray | no depth, `Incomplete` |
| Below threshold | the chunk's recorded convention says this code was below threshold | zero depth, no flag: measured, no echo |
| Missing | the chunk's recorded convention says this code is missing (nodata / range folded) | zero depth, `Incomplete` |
| Sentinel, no convention | `momentEncoding == Unknown` (legacy spill file) | zero depth, no flag: the gap cannot be claimed |

Codes 0 and 1 mean "below threshold" and "missing", but the two decoded formats
disagree on which is which: NEXRAD Message 31 packs 0 = below threshold and
1 = range folded, while ODIM_H5 packs 0 = missing (nodata) and 1 = below
threshold. `DecodedChunk::momentEncoding` now records which format produced the
gates and `momentGateState()` classifies one code (contract and fixture evidence:
`docs/moment-sentinels.md`). `accumulateRainfall` follows it: a below-threshold
gate is a measured dry gate (zero depth, no flag), a missing gate is a coverage
gap (zero depth, `Incomplete`, which downgrades the product to Marginal), and a
chunk with no recorded convention keeps the older, weaker reading — zero depth
and no flag, because the gap cannot be claimed. Depth is unaffected in all three
cases (zero either way); what changed is that a gap is now reported instead of
being silently read as dry.

A volume that reaches products through `MainWindow::acceptDecodedChunk` still
reports `Unknown` until that merge copies the field (card `t_f84c4bc9`), so the
live path keeps the legacy reading until that lands. The closed-form test for
the same gate under both codes is `testRainfallSentinelConvention` in
`core_tests`.

Product quality (about whether the number may be read as a total):

- **Invalid** — fewer than two usable volumes, no reference geometry, or zero
  credited seconds.
- **Poor** — credited coverage under 50% of the window, or clutter /
  non-meteorological exclusions above 5% of rain-bearing gates.
- **Marginal** — fewer than four volumes, any gap-clamped interval, any
  `Incomplete` gate, or any `NoVerticalCheck` gate.
- **Good** — none of the above.

Beam-height, range and hail flags are provenance, not quality downgrades: they
say *where* the estimate is weak and are counted separately for the UI.

## Where it breaks down

Unscored, but already known from the method. These are the reasons the product
cannot be trusted for a while:

1. **Z-R scatter.** One relation cannot cover stratiform, tropical, orographic
   and convective rain; published Z-R error is routinely ±30-50% in the
   instantaneous rate, and it compounds over an accumulation window.
2. **Hail.** The 53 dBZ cap stops a hail core from minting hundreds of mm, but
   the cap is not a hail correction: a hail day reads as heavy rain.
3. **Bright band.** Far gates on the 0.5 degree tilt sample the melting layer,
   which inflates Z by several dB. At 150 km that beam is already about 3.0 km
   MSL (2.6 km above the antenna).
4. **Beam height.** Above about 186 km the lowest beam is over 4 km MSL and no
   depth is credited at all; between roughly 108 and 186 km the depth is a
   mid-level estimate flagged `BeamHigh`, not surface rain.
5. **Sampling.** Left hold assumes the rate is constant between volumes. Rain
   that grows or decays within a 5-6 minute interval is misattributed, and the
   newest volume contributes nothing, so the product is a **lower bound** at
   the leading edge by construction.
6. **Static clutter that survives the vertical test.** A persistent building or
   tower return with real echo above it is indistinguishable from steady rain
   without a long-term clutter map. The guard removes the shallow AP signature,
   not every artefact.
7. **Partial coverage.** Missing volumes, truncated rays and range-folded gates
   keep the gates they touch out of the total. The summary reports how many;
   a small number is normal, a large number means the total covers less ground
   than it appears to.
8. **C-band OPERA.** Attenuation and lower Nyquist structure differ from S-band;
   the same relation is used, and it has not been compared on European data.
9. **No gauge truth yet.** Everything above is method criticism, not a measured
   error. The point of the plan below is to replace it with numbers.

## Comparison plan

The exact services below were checked live on 2026-10-20 (bucket prefix listing
returned date partitions; the IEM endpoints returned data rows). None of them
are wired into the app yet — this is the plan, not a claim.

1. **MRMS 1-hour QPE (radar-vs-multisensor).**
   `https://noaa-mrms-pds.s3.amazonaws.com/CONUS/MultiSensor_QPE_01H_Pass2_00.00/YYYYMMDD/MRMS_MultiSensor_QPE_01H_Pass2_00.00_YYYYMMDD-HHMMSS.grib2.gz`
   (public, no auth; prefix verified). Match the accumulation clock hour to the
   product valid time, compare gate to nearest MRMS cell, and report bias, MAE
   and correlation **stratified by range band** (< 50, 50-100, 100-150,
   150-200 km) so the beam-height and bright-band errors are visible instead of
   averaged away.
2. **ASOS 1-minute precipitation (station truth).**
   `https://mesonet.agron.iastate.edu/cgi-bin/request/asos1min.py?station=OKC&year1=…&month1=…&day1=…&hour1=…&minute1=…&year2=…&vars=precip&format=csv`
   (station id **without** the leading K; verified). Pick the nearest station
   within 20 km of the gate, compare the same clock hour. **Confirm the units on
   a rainy hour before scoring** — this endpoint's precip unit has not been
   verified, and a mm/inch mix-up is a 25x error.
3. **METAR hourly precipitation (coarse fallback).**
   `https://mesonet.agron.iastate.edu/cgi-bin/request/asos.py?station=KOKC&data=p01i&…&tz=UTC&format=csv`
   (verified; `p01i` is inches). Use where 1-minute data is missing.
4. **Dry-day AP test (pass/fail, no gauge needed).**
   Five clear-air volumes on a dry day. Pass requires: **no gate above 2.0 mm**
   and ≥ 95% of gates below 0.5 mm, with any residue explained by
   `ClutterSuspect` / `NonMeteorological` counts. This is the test that decides
   whether the guard works; a dry day that accumulates rain fails the product
   outright, whatever the wet-day scores say.
5. **Hail cap test (pass/fail, no gauge needed).**
   On a hail case, no gate may exceed `53 dBZ` rate × credited intervals
   (103.8 mm/h × hours). This proves the cap is applied, not that the total is
   right.

Scoring gates before the UI may state a value as an estimate rather than a
number:

- ≥ **10 radar-hours** with real rain (max accumulation ≥ 5 mm) scored against
  MRMS or gauges, plus ≥ **5** dry-day volumes under the AP test.
- Median absolute hourly difference ≤ **50%** of the MRMS hour, bias within
  ±30%, and the range-band table must be published — including the bands where
  it fails.
- The dry-day test must pass before any wet-day number is shown, because a
  contaminated total is worse than no total.

## What the UI may say (when it lands)

No surface exists yet; the core function is only reachable from `core_tests`.
When a panel or pop-out is wired:

- Label it as an estimate with its window and coverage, e.g.
  `Z-R estimate — 58 min scored of 60 min, 20 volumes` in the same style as the
  SCAN age block. Never a bare `12.4 mm`, never "rainfall" as a measured
  quantity, never a diameter or warning style.
- Show the quality flag and the dominant caveat (`Marginal: 37 gates within
  9 km where the column guard could not run`) instead of hiding it.
- A stale history must say so: if `coveredSeconds` is well short of the
  requested window, the number is a partial total and must be labelled as one.

## Scoring log

Empty by design. Add one row per scored case; do not delete rows that failed.

| Date | Case | Radar | Window | Truth source | Metric | Result | Notes |
|---|---|---|---|---|---|---|---|
| — | — | — | — | — | — | — | not scored yet |

## Change history

- 2026-10-20 — engine, AP/clutter guard, quality flags and closed-form tests
  added; this record created with zero scored cases. No UI surface yet; the
  display wiring is a follow-up.
- 2026-10-20 — limitation 8 (format sentinel ambiguity) struck. The decoder now
  records the sentinel convention on the chunk (`DecodedChunk::momentEncoding`,
  `momentGateState()`, see `docs/moment-sentinels.md`) and `accumulateRainfall`
  consumes it: a `Missing` gate raises `Incomplete` (and downgrades quality to
  Marginal), a `BelowThreshold` gate stays zero depth with no flag, and a chunk
  with no recorded convention keeps the old reading. Closed-form coverage:
  `testRainfallSentinelConvention` (same gate missing in one volume, below
  threshold in the next; ODIM's swapped codes; legacy `Unknown`). The live path
  reaches products as `Unknown` until `MainWindow::acceptDecodedChunk` copies the
  field (card `t_f84c4bc9`), so this is a decoder-and-core fix, not yet a live
  product improvement.
