# Rainfall accumulation (Z-R, R(Z,ZDR), R(KDP)) — validation record and limits

Status: **not validated. Zero cases scored.** All three relations share this
record. R(KDP) additionally has **no real-data run through the engine**: no
openly published product checked for this card carries a KDP moment, and NEXRAD
Level II Message 31 has no KDP block (see "Real-data check").

The engine exists (`accumulateRainfall` in `src/core/derived_products.cpp`) and
is exercised by closed-form core tests, but it has **never been compared with a
gauge or with MRMS**. The app shows it as an unvalidated estimate with its
window and coverage (see "What the UI says"), never as a measurement: nothing
may be presented as one until the gates in "Comparison plan" below are met and
logged in "Scoring log". Until then every number it produces is a lower-bound
guidance estimate derived from radar moments, not observed rainfall.

This file is the record that makes the absence of validation explicit. It is
required by the honesty bar: no precision claim without a scored record.

## What the engine computes

One rain-rate relation per gate on the lowest reflectivity tilt of each volume.
The relation, its inputs and its range limits are configuration
(`RainfallConfig`); the relation functions are the single source of each
formula so tests and any future UI cannot drift apart.

| Relation | Formula | Needs (besides reflectivity) | Source |
|---|---|---|---|
| `MarshallPalmer` (default) | `R = (Z / 300)^(1/1.4)` | none | Marshall–Palmer, maritime default |
| `Zdr` — R(Z,ZDR) | `R = 0.0142 · Z^0.770 · Zdr^-1.67`, `Zdr = 10^(ZDR/10)` | differential reflectivity (ZDR) | Ryzhkov, Giangrande & Schuur (2005), NSSL report, Eq. (3); Oklahoma DSD statistics, equilibrium drop shapes |
| `Kdp` — R(KDP) | `R = 44.0 · \|KDP\|^0.822 · sign(KDP)`, KDP in deg/km | specific differential phase (KDP) | WSR-88D operational dual-pol QPE relation (Ryzhkov et al. 2005; Giangrande & Ryzhkov 2008; NWS Surface Precipitation Rate description) |

All three are **S-band** relations. The app also reads C-band OPERA volumes;
applying these coefficients there is a known, unscored extrapolation
(limitation 8 below). Reflectivity is required for every relation even when it
is not part of the rate: it is the only source for the AP/clutter guard's
vertical columns.

Per-relation limits and clamps (all counted, none silent):

| Limit | Default | Effect |
|---|---|---|
| Z hail cap (Z-R, R(Z,ZDR)) | 53 dBZ | Z clamped before the relation, `HailCapped` |
| ZDR valid range (R(Z,ZDR)) | −1 … 8 dB | input clamped, `InputClamped`; a gate needing this is suspect |
| KDP noise floor (R(KDP)) | 0.1 deg/km | below it the gate contributes no rate (light-rain noise) |
| KDP valid range (R(KDP)) | ≤ 20 deg/km | magnitude clamped, `InputClamped` |
| Rate ceiling (all) | 150 mm/h | rate clamped, `RateCapped`; binds on KDP spikes, not on the Z caps |

The 150 mm/h ceiling is the operational R(KDP) cap. Z-R and R(Z,ZDR) reach at
most 103.8 mm/h at the 53 dBZ hail cap, so the ceiling only bites on a KDP
spike. ZDR outside its range is clamped rather than dropped: the value is
reported (`inputClampedGateCount`) so a product made mostly of clamped gates is
visibly not a clean estimate.

Rain is integrated by **left hold** between volume times: the rate measured at
volume *i* is credited over `[t_i, t_i+1)`. The last volume is credited nothing
unless the caller passes a closing clock. An interval longer than 600 s means
volumes are missing, so it is credited as 600 s and every gate it touched is
flagged `GapClamped` — a multi-hour gap is never poured through one rate.

Output is per gate in **0.1 mm** (a tipping-bucket resolution, not a claim of
rainfall accuracy), plus a summary of what the number is actually made of:
samples used, window length, credited seconds, gap-clamped intervals, and
per-flag gate counts. The reference grid is the newest volume's lowest
reflectivity tilt for **every** relation, so the three relations line up gate
for gate; each ray reports the reference chunk's ray index.

## Relation availability: missing dual-pol is missing

The newest volume decides whether a relation can be computed at all. If it does
not carry the relation's inputs, `accumulateRainfall` returns an invalid grid
with `relationAvailable == false` and `relationUnavailableReason` naming the
missing product; it **never falls back to Z-R**. The same rule applies to a
volume that carries no reflectivity (the guard cannot run).

An older volume inside the window that lost the moment is a coverage gap, not a
substitution: every gate it would have covered gets zero depth and
`Incomplete` (which downgrades quality to Marginal). It is never filled from
another relation.

The dual-pol moment is read at the reflectivity source gate only when it shares
that ray's range table (same first gate and spacing). A volume assembled from
separate, unmerged per-moment rays (a SCAN assembly that appends one ray per
moment) therefore reports `Incomplete` gates rather than reading a value from
the wrong range. The app's live OPERA path currently downloads DBZH only, so
that limitation is not exposed until a dual-pol moment is wired into the feed.

R(KDP) still applies the 5 dBZ reflectivity floor as a consistency gate: where
reflectivity is present and says the gate is dry, KDP cannot mint rain there.
Where reflectivity is missing entirely at a gate, the KDP rate is kept (KDP is
the rate input) and the gate is flagged `NoVerticalCheck`, because the column
guard could not run. Reflectivity below threshold is always a measured dry gate.

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
| Hail | Z > 53 dBZ (Z-R, R(Z,ZDR)) | rate clamped at 53 dBZ, `HailCapped` |
| Relation input | R(Z,ZDR): ZDR < −1 or > 8 dB | input clamped, `InputClamped` |
| Relation input | R(KDP): \|KDP\| < 0.1 deg/km | no rate: light-rain noise floor |
| Relation input | R(KDP): \|KDP\| > 20 deg/km | magnitude clamped, `InputClamped` |
| Rate ceiling | any relation: rate > 150 mm/h | rate clamped, `RateCapped` |
| Structural gap | no ray at the azimuth, or the gate lies beyond that sample's ray | no depth, `Incomplete` |
| Dual-pol gap | R(Z,ZDR): ZDR missing or unusable at a rain-bearing gate | no depth, `Incomplete` |
| Dual-pol gap | R(KDP): KDP missing at a gate | no depth, `Incomplete` |
| Dual-pol dry | R(KDP): KDP below threshold | zero rate, no flag: measured, no phase shift |
| Moment absent | the newest volume carries no ZDR / KDP (or no reflectivity for the guard) | relation unavailable, reason named, no grid |
| Moment absent, older volume | a sample inside the window lost the moment | zero depth, `Incomplete`; never Z-R |
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

The live path is covered too: `MainWindow::acceptDecodedChunk` copies
`momentEncoding` into the assembled volume (card `t_f84c4bc9`), so a volume that
reaches products through the app is classified the same way the decoder wrote
it. The closed-form test for the same gate under both codes is
`testRainfallSentinelConvention` in `core_tests`.

Product quality (about whether the number may be read as a total):

- **Invalid** — fewer than two usable volumes, no reference geometry, or zero
  credited seconds.
- **Poor** — credited coverage under 50% of the window, or clutter /
  non-meteorological exclusions above 5% of rain-bearing gates.
- **Marginal** — fewer than four volumes, any gap-clamped interval, any
  `Incomplete` gate, or any `NoVerticalCheck` gate.
- **Good** — none of the above.

Beam-height, range, hail, input-clamp and rate-cap flags are provenance, not
quality downgrades: they say *where* the estimate is weak and are counted
separately for the UI. ZDR/KDP clamping and the rate ceiling do not by
themselves make a window Marginal; a product made mostly of clamped gates is a
sign to distrust it, which is why the count is reported next to the depth.

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
   the same relation is used, and it has not been compared on European data. All
   three coefficient sets are S-band; R(KDP) in particular has a different
   coefficient/exponent at C band, so a C-band OPERA KDP volume would be read
   with the wrong relation until band-specific coefficients exist.
9. **R(Z,ZDR) in mixed phase and hail.** Z and ZDR are both affected by hail;
   ZDR near 0 dB in a hail core makes the ZDR correction vanish, so the relation
   falls back toward Z-R behaviour exactly where Z is least trustworthy. The
   53 dBZ cap bounds it; it does not correct it. Melting-layer ZDR bias and a
   1 dB ZDR calibration error also move the rate (about 32% per dB at 1 dB
   ZDR, from the 10^(-0.167·ZDR) factor), and ZDR is noisy at low Z.
10. **R(KDP) at low rates.** The relation is noisiest precisely where most rain
    volume is: below about 5-6 mm/h KDP estimates scatter and go negative, and
    the 0.1 deg/km floor turns that into a dry gate. It is also only as good as
    the KDP retrieval (smoothing, ΦDP unfolding, backscatter phase removal),
    which this app does not perform: it consumes a KDP moment if the volume
    carries one. NEXRAD Level II Message 31 has no DPKP block, so the relation
    is unavailable on US Level II data by construction; KDP exists only in
    ODIM-style volumes (and in NEXRAD Level III product 163, which the app does
    not decode).
11. **Moment geometry.** The relation reads ZDR/KDP from the same ray and range
    table as reflectivity. A volume whose dual-pol moment is a separate
    unmerged ray (per-moment SCAN assembly) reports `Incomplete` instead of a
    value, so a real window can look emptier than the moments in it.
12. **No gauge truth yet.** Everything above is method criticism, not a measured
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

Each relation is scored **separately on the same windows**: a Z-R table does not
validate R(Z,ZDR), and a wet R(Z,ZDR) score does not validate R(KDP). R(KDP)
additionally needs a real KDP source before it can enter the plan at all — this
app currently decodes KDP only from ODIM_H5 volumes, and no openly published
KDP-carrying volume was found for this card (see "Real-data check").

## Real-data check (not a validation)

Run 2026-10-22 through `cappedweather_core_tests`. Times are synthetic (the same
real gates handed in four times, 300 s apart); the gates are real archive data,
so the numbers show what the relation does on a real radial, not that it is
right.

Source: `tests/fixtures/level2_radial_ktlx_20130520_vcp12.bin`, a real KTLX
Message 31 radial from the 2013-05-20 Moore tornado volume (0.68 deg, real
reflectivity + ZDR + RHOHV).

| Relation | Peak gate | Mean (scored gates) | Gates with depth | Flags |
|---|---|---|---|---|
| Z-R | 0.4 mm | 0.267 mm | 6 | none on the rain gates (weak echo, vertical guard not triggered) |
| R(Z,ZDR) | 0.4 mm | 0.150 mm | 6 | `InputClamped` 7 (ZDR outside −1…8 dB at rain gates), `RateCapped` 0 |
| R(KDP) | unavailable | — | — | reason: `R(KDP) needs Specific differential phase on the newest volume's lowest reflectivity tilt; that tilt does not carry it` |

The small totals are honest: one radial of weak rain, 0.1 mm storage. The
same radial's Z-R output is unchanged by this change (the closed-form tests pin
31 tenths for the 40 dBZ case and 110 for KDP = 1 deg/km).

**KDP has no real-data run through the engine.** The card's premise that the
ODIM DENE fixtures carry KDP is not what the repo contains: `odim_deneu_dbzh.h5`,
`odim_deneu_uzdr.h5`, `odim_deneu_urhohv.h5` and `odim_deneu_uphidp.h5` decode
real DWD sweeps for DBZH, UZDR, URHOHV and UPHIDP; there is no KDP file. The
only KDP-bearing container in `tests/fixtures/` is
`odim_specshape_dualpol_3ray.h5`, which is constructed byte by byte
(`make_odim_dualpol_specshape.py`), so it proves the decode/load path and
nothing about the geophysics. Sources probed on 2026-10-22 and found to publish
**no** KDP moment: DWD `sweep_vol_{z,zdr,rhohv,phidp}` (no KDP product); the
first ~11.9k keys of the OPERA `openradar-24h` bucket for the day were
DBZH/TH/VRAD/VRADH only, and NL PVOL carries ZDR/RHOHV/PHIDP but no KDP; the
sampled FMI volume carried TH only; CHMI opendata publishes Z, U, V, W, ZDR,
RHOHV and PHIDP. NEXRAD Level II Message 31 has no DPKP block; NEXRAD Level III
product 163 does carry KDP but this app has no Level III decoder.

Consequence: R(KDP) ships as a **closed-form-tested, unscored** relation whose
availability on real volumes is proven only by the fail-closed test above. The
first real KDP volume (ODIM with a KDP moment, or a Level III 163 reader)
must be scored before any KDP number is shown as an estimate.

## What the UI says

The readout is in the control dock's Display tab (`Rainfall (Z-R estimate)`),
next to the SCAN age block. It is computed on a worker thread from the active
session's frame times and re-scored whenever the window changes (a new volume,
a radar switch, an archive load); resident frames are borrowed and spilled
frames are faulted in from disk by path, so the pass never blocks the GUI
thread or shifts the RAM budget while it runs. The `Details` pop-out (and the
command palette's `Analysis: rainfall estimate details`) carries the per-flag
provenance table and the caveats.

The dock readout is still **Z-R only** and its wording is unchanged by the
dual-pol engine work: selecting R(Z,ZDR) or R(KDP) there is a follow-on in
`main_window.cpp` (manager's lane). The engine already exposes
`RainfallSummary::relation`, `relationAvailable` and
`relationUnavailableReason` for that readout, and
`rainfallRelationName()` for its label.

The dock block is three lines, in the SCAN age block's style:

```text
Z-R estimate — peak 12.4 mm
55 min credited of 55 min window · 12 volumes
Quality: Good — window fully credited, guard ran where it mattered
```

- Line 1 names the product and the **peak gate** total (the deepest column in
  the window) — never a bare `12.4 mm`, never "rainfall" as a measured
  quantity. An unscorable window reads `Z-R estimate — unavailable` instead of
  `0.0 mm`, which would read as a measured dry spell.
- Line 2 is the window and the coverage: `coveredSeconds` credited against
  `windowSeconds` plus the usable volume count. The newest volume is not
  credited (left hold), so the total is a lower bound by construction; the
  tooltip and the pop-out both say so.
- Line 3 is the quality flag plus its dominant caveat. Inside Marginal the
  order is: a gap-clamped interval (it shortens credited time), then gates
  where the column guard could not run, then gates missing an interval, then a
  short window. Poor reports the coverage share first, the clutter/CC
  contamination share when coverage is fine.
- The pop-out adds the peak gate's azimuth and range, the mean over scored
  gates, the reference grid geometry, the credited window as UTC times, frames
  handed in / rejected samples / unreadable spills, the relation and caps in
  force, and the unvalidated status pointing back at this file.

Nothing in the readout claims a measurement, and the dock never shows a
precision claim. The scoring gates below are still the bar: until they pass,
the wording stays "Z-R estimate", the coverage ratio stays visible, and no
percentile, gauge agreement, or "storm total" claim may appear.

## Scoring log

Empty by design. Add one row per scored case; do not delete rows that failed.

| Date | Case | Radar | Window | Truth source | Metric | Result | Notes |
|---|---|---|---|---|---|---|---|
| — | — | — | — | — | — | — | not scored yet |

## Change history

- 2026-10-22 — dual-pol relations added: `RainfallRelation` (`MarshallPalmer`,
  `Zdr`, `Kdp`) in `RainfallConfig`, with `zzdrRateMillimetersPerHour` /
  `kdpRateMillimetersPerHour` as their single sources. All three reuse the same
  window, reference grid, AP/clutter guard, gap clamp and provenance flags; a
  relation whose newest-volume input is missing fails closed with
  `relationAvailable=false` and a reason instead of falling back to Z-R.
  New flags/counts: `InputClamped` (ZDR −1…8 dB, KDP ≤ 20 deg/km) and
  `RateCapped` (150 mm/h ceiling). Closed-form tests:
  `testDualPolRateRelations`, `testRainfallZdrAccumulationClosedForm`,
  `testRainfallKdpAccumulationClosedForm`, `testRainfallDualPolUnavailable`,
  `testRainfallDualPolGuardParity`, plus a real-radial check
  (`testRainfallDualPolRealArchive`). Still zero scored cases; R(KDP) has no
  real-data run because no openly published KDP volume was found (DENE has no
  KDP fixture; NEXRAD Level II has no KDP block).
- 2026-10-20 — UI readout landed: control-dock `Rainfall (Z-R estimate)` block
  (peak, credited window, volume count, quality flag + dominant caveat) and a
  Details pop-out with the per-flag provenance table. The pass runs off the GUI
  thread over the session window using `RadarFrame::dataTime` (spilled frames
  faulted in by path, never by mtime). Scoring plan unchanged: still zero
  scored cases, so the wording stays estimate-only.
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
  carries the same convention since `MainWindow::acceptDecodedChunk` copies the
  field into the assembled volume (card `t_f84c4bc9`, merged with this chain),
  covered by `tests/ui_smoke_test.cpp::testMomentEncodingMerge`.
