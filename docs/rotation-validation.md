# Rotation validation — archived tornado cases

Status: tuned and promoted (v3). 68 volumes across 3 outbreaks scored; shipped
defaults: `min-reflect 35`, over-unfold veto 120 m/s, conditional unfold + 90 m/s
physical cap, range-relaxed couplet gate (5 m/s − 0.02/km, floor 2), same-sign OFF,
split-cut ROT (own lowest tilt per product, native velocities).

## Why this exists

The live detector (`src/core/circulation_detector.cpp:61` gate-to-gate couplets +
`src/core/radar_lightning_fusion.cpp:38` thresholded shear objects, painted by
`src/app/radar_map_canvas.cpp:2243` / `:2290`) was tuned on synthetic volumes
only (`tests/core_tests.cpp:973`). Your proposal is the right fix: tune against
real tornadic scans and score like the lightning work (`docs/validation-2025-05-16.md`,
`tools/m5_verify.py`).

GR-style "gridded" rotation is a dense azimuthal-shear wash. We are not there yet:
today we emit sparse TVS / MESO / ROT markers. This harness scores those markers
first, so the grid upgrade has a number to beat.

## Cases (`data/rotation_cases/cases.json`)

| Case | Radar | Window (UTC) | Truth |
|---|---|---|---|
| Mayfield Quad-State EF4 | KPAH 37.0683,-88.7717 | 2021-12-11 01:00–05:00 | `211210_rpts.csv` + IEM TOR polygons |
| Moore EF5 | KTLX 35.3331,-97.2778 | 2013-05-20 19:00–21:00 | `130520_rpts.csv` + IEM TOR polygons |
| Hackleburg outbreak | KHTX 34.9306,-86.0822 | 2011-04-27 19:00–23:00 | `110427_rpts.csv` + IEM TOR polygons |

Volumes are evenly sampled to the per-case `max_volumes` cap (same rule as the
in-app AWS dialog). Pre-2015 objects are outer-`.gz` wrapped in the bucket;
the downloader gunzips them since `decodeLevel2Archive` takes raw Archive-II.

## Run it

```bat
:: 1. volumes (~600 MB total for all three cases)
python tools/download_rotation_cases.py --cases data/rotation_cases/cases.json --output data/rotation_cases

:: 2. SPC ground truth (small CSVs)
python tools/fetch_spc_reports.py --cases data/rotation_cases/cases.json --output data/rotation_cases

:: 3. TOR warning polygons (one per case window)
python tools/fetch_verification_truth.py --start 2021-12-11T01:00:00Z --end 2021-12-11T05:00:00Z --output data/rotation_cases/20211211-kpah-mayfield/warnings.geojson
python tools/fetch_verification_truth.py --start 2013-05-20T19:00:00Z --end 2013-05-20T21:00:00Z --output data/rotation_cases/20130520-ktlx-moore/warnings.geojson
python tools/fetch_verification_truth.py --start 2011-04-27T19:00:00Z --end 2011-04-27T23:00:00Z --output data/rotation_cases/20110427-khtx-hackleburg/warnings.geojson

:: 4. replay (needs Qt on PATH for the Archive-II decoder)
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%
build\Release\rotation_replay.exe data\rotation_cases\20130520-ktlx-moore --lat 35.3331 --lon -97.2778 --elev 389 --csv > data\rotation_cases\20130520-ktlx-moore\detections.csv

:: 5. score (filter reports to the radar domain; allow slightly-post-touchdown detections with --lead-min -10)
python tools/rotation_verify.py --detections data\rotation_cases\20130520-ktlx-moore\detections.csv --reports data\rotation_cases\20130520-ktlx-moore\spc_rpts.csv --convective-date 2013-05-20 --warnings data\rotation_cases\20130520-ktlx-moore\warnings.geojson --radar 35.3331,-97.2778 --max-range-km 150 --radius-km 15 --lead-min -10 --lead-max 45
```

`rotation_replay` flags for threshold sweeps: `--nyquist`, `--meso-dv`, `--tvs-dv`,
`--min-reflect`. `rotation_verify` flags: `--kinds TVS,MESO,ROT`, `--radius-km`,
`--lead-min/max`, `--radar LAT,LON --max-range-km`.

## Baseline: Moore, 20 volumes, defaults + over-unfold veto

Full KTLX window 19:00–21:00Z 2013-05-20 (20 sampled volumes), `rotation_replay`
defaults (`--meso-dv 10 --tvs-dv 25 --min-reflect 20 --max-dv 120 --nyquist 28`):

- detections: TVS=43 MESO=59 ROT=0 (102 total, ~5/vol)
- vs 9 TOR warning polygons (15 km, lead [-10,+45] min): **POD 0.89, FAR 0.92, CSI 0.08**,
  median lead +26 min — finds the tornadoes but drowns them in false alarms.
- vs 2 domain SPC reports: POD 0.50, FAR 0.99, CSI 0.01, hit lead +13 min.
- the 223 m/s single-gate monster is gone (veto works); remaining FAR is
  far-field MESO (e.g. delta-V 27 at 100+ km in 20 dBZ echo).

A threshold sweep (`tools/rotation_sweep.py`, ranked by warning CSI like
`tools/run_m5_suite.py`) is running: `--meso-dv 10,14,18 × --tvs-dv 25,30 ×
--min-reflect 20,30,35`. Expectation: raising `min-reflect` toward 30–35 cuts
stratiform shear at small POD cost; raising `meso-dv` trims weak far-field
couplets. Winner becomes the new default; this section gets rewritten with its
numbers plus the KPAH/KHTX cases once their downloads land.

### Moore sweep result (20 vols, 9 TOR warnings)

| meso_dv | min_refl | n | warn POD | warn FAR | warn CSI | spc CSI |
|---:|---:|---:|---:|---:|---:|---:|
| 18 | 35 | 37 | 0.56 | 0.86 | **0.122** | 0.000 |
| 18 | 30 | 45 | 0.56 | 0.89 | 0.102 | 0.022 |
| 10 | 35 | 71 | 0.78 | 0.90 | 0.096 | 0.014 |
| 10 (base) | 20 (base) | 102 | 0.89 | 0.92 | 0.078 | 0.010 |

(`tvs-dv` 25 vs 30 barely matters — the MESO gate + reflectivity guard do the
work; TVS threshold rarely binds.) Strict `meso-dv 18` wins Moore warnings but
collapses Mayfield POD (0.09→0.03), so the winner is picked on all three cases.

### All-cases result (68 vols; warnings 15 km / SPC reports 15 km, lead [-10,+45] min)

| Config | Moore warn CSI/POD/FAR (9) | Moore SPC (2) | Mayfield warn (80) | Mayfield SPC (24) | KHTX warn (154) | KHTX SPC (90) |
|---|---|---|---|---|---|---|
| base: meso10/ref20 | 0.078 / 0.89 / 0.92 | 0.010 | 0.077 / 0.09 / 0.61 | 0.135 | 0.120 / 0.16 / 0.68 | 0.063 |
| meso10/ref30 | 0.084 / 0.78 / 0.91 | 0.012 | 0.068 / 0.07 / 0.57 | 0.152 | **0.124** / 0.14 / 0.43 | 0.076 |
| **promoted: meso10/ref35** | 0.096 / 0.78 / 0.90 | 0.014 | 0.060 / 0.06 / 0.38 | **0.185** | 0.121 / 0.13 / 0.35 | **0.080** |
| strict: meso18/ref35 | **0.122** / 0.56 / 0.86 | 0.000 | 0.025 / 0.03 / 0.00 | 0.083 | 0.094 / 0.10 / 0.25 | 0.068 |

Detections: base 199 → ref30 132 → **ref35 110** (−45%). Warning hits: 40 → 34 → **32** (−20%).
SPC CSI sums: 0.208 → 0.240 → **0.279**. Median leads: Moore +26 min, Mayfield +7 min, KHTX +8 min.

Promoted (`src/core/circulation_detector.hpp:36`): `minReflectivityDbz 20→35`,
`maxDeltaVelocityMetersPerSecond 120` (over-unfold veto). `meso-dv` stays 10 —
18 wins Moore alone but blinds Mayfield/Hackleburg far-field. `tvs-dv` stays 25.

### v2–v3: velocity + gate + ROT fixes (TVS/MESO, unfiltered warnings)

| Config | Moore warn (9) CSI / n | Moore SPC (2) | Mayfield warn (80) | Mayfield SPC (24) | KHTX warn (154) | KHTX SPC (90) |
|---|---|---|---|---|---|---|
| v1 promoted (ref35, always-unfold) | 0.096 / 71 | 0.014 | 0.060 / 8 | 0.185 | 0.121 / 31 | 0.080 |
| v2 conditional-unfold + cap, ssb off (mg5 fixed) | **0.108** / 63 | 0.016 | 0.061 / 7 | 0.148 | 0.120 / 32 | 0.080 |
| v3 range-relaxed gate (5 − 0.02/km, floor 2) | 0.067 / 103 | 0.010 | 0.070 / 12 | 0.161 | **0.197** / 59 | **0.120** |
| v3 + fixed mg3 (no slope) | 0.051 / 134 | 0.007 | 0.080 / 15 | 0.147 | 0.179 / 70 | 0.111 |

Warning-aggregate CSI (hits / hits+misses+FA): v1 0.102 → mg3 0.116 → **v3 0.130**.
SPC-aggregate CSI: v1 0.069 → mg3 0.070 → **v3 0.082**. Median leads: Moore +28 min,
Mayfield +9 min, KHTX +23 min.

### v3b: domain-filtered warnings (the Mayfield map fix)

The Mayfield map shows the flaw: 80 TOR polygons span Little Rock → Chicago →
Nashville, most hundreds of km from KPAH, and single-radar detections can never
match them. `rotation_verify.py` and `rotation_sweep.py` now domain-filter warnings
the same way as SPC reports (keep a polygon only if any vertex is within 150 km
of the radar). Same v3 detections, honest denominators:

| Case | Warn events 80/154 → in-domain | POD | FAR | CSI |
|---|---|---|---|---|
| Moore | 9 → 8 | 0.875 | 0.93 | 0.067 |
| Mayfield | 80 → 32 | **0.19** (was 0.075) | 0.50 | **0.158** (was 0.070) |
| Hackleburg | 154 → 66 | **0.47** (was 0.23) | 0.47 | **0.330** (was 0.197) |

KHTX at 0.33 CSI / 0.47 POD with 0.47 FAR is a respectable tornado-guidance
operating point. Remaining misses are now mostly real: QLCS spin-ups with shallow
/ short-lived shear below the 2-tilt + 2500 m depth gates, and far-field couplets
smoothed below the gate by beam broadening — plus a few over-unfolded volumes.
Chasing those with lower thresholds trades 1 hit for 10+ false alarms on every
sweep tried so far.

### Mayfield miss diagnosis (`--diagnose`: couplets / columns / single / weak / shallow / wide)

Per-volume stage counts on KPAH 01:03–02:46Z (quad-state tornado lifetime):
couplets exist every volume (1–16, delta-V 26–28 m/s, `weak=0` always) but
columns die as `single` (1 tilt only, 1–7/vol) or `shallow` (depth < 3000 m).
E.g. 02:23Z: 11 couplets → 7 columns → 5 single + 1 shallow → 1 det.
The shear is real and strong; it never stacks vertically within 5 km across
2+ tilts. Loosening to depth 1500 m + align 8 km converts some (Mayfield 12 →
31 dets, 6 → 8 hits) but FAR 0.50 → 0.74 and CSI 0.158 → 0.145 — the extra
columns are mostly noise. The principled fix is *temporal* persistence (live
`persistCirculations` already needs 2 repeat volumes before drawing; replay
scores raw single-volume output), not lower single-volume gates — plus
spatially-consistent dealiasing so upper-tilt couplets stop being missed.

Shipped as v3 defaults: conditional unfold holds Moore POD with fewer dets; the
range slope trades +40 Moore FAR dets for +16 warning / +8 SPC hits downrange,
where beam smoothing weakens couplets. Same-sign stays OFF (Moore CSI 0.108 →
0.062 with zero new hits); single-tilt TVS stays opt-in (Mayfield SPC 0.147 →
0.109 when enabled).

ROT (native velocities after the dealiased-span fiasco): Moore 251 dets, POD 1.0,
FAR 0.96, CSI 0.036 — unselective at storm scale; spans now physical (max 61,
median 52) but the gate can't tell mesos from tornadoes. SHEAR prototype (LLSD
grid, `src/core/shear_grid.cpp`): Moore 92 dets at 0.025 threshold, POD 0.44,
CSI 0.041 — fires, but far below TVS/MESO and likely poisoned by unfold
discontinuities. Both stay out of the promoted path; they need persistence +
spatially-consistent dealiasing before they can beat sparse couplets.

### Honest limits (read before quoting skill)

- FAR is still 0.47–0.93 depending on case: these are guidance markers, never warnings.
- Mayfield warning POD was 0.06 before the domain filter because 80 TOR polygons
  span four states while matching radius is 15 km — geography, not just detector.
  Domain-filtered (32 in-range polygons) it is 0.19; SPC reports (0.161 CSI) remain
  the fairer truth there. See v3b.
- ROT shear objects fired **0 times in 68 volumes** in the v1 harness — root cause
  found: NEXRAD split cuts put reflectivity-only surveillance (elev #1) below the
  lowest Doppler tilt (elev #2), and the fusion code took the single lowest elevation
  carrying *either* product, so `velocitySamples` was always 0. Fixed by taking each
  product from its own lowest tilt (`src/core/radar_lightning_fusion.cpp`) with a
  split-cut regression test. But revived ROT is unselective (~10/vol, CSI ~0.04):
  storm-scale span is not tornado-scale shear.
- Dealiased ROT spans hit 60–200 m/s on real volumes (adjacent rays unfold to
  different Nyquist intervals), so ROT stays on native velocities (span capped at
  2*Nyquist by physics of folding). TVS/MESO keep the conditional-unfold +
  90 m/s physical cap (`src/core/velocity_processing.cpp`).
- Same-sign (storm-motion-biased) couplets hurt on Moore: ssb=1.8 adds 48 dets with
  zero new hits (CSI 0.108 → 0.062). Default is now OFF (`sameSignBoost = 0.0`).
- KHTX needed the `F-Scale` hyphen fix in `tools/rotation_verify.py` (pre-2012 SPC
  spelling) — without it the whole 2011 truth set silently scored 0 events.
- "Perfect" is not on the table: single-radar gate-to-gate couplets cannot resolve
  every QLCS spin-up. Next real gains: azimuthal-persistence dealiasing, then the
  dense LLSD shear grid as a first-class product (your screenshot wash).

## Next steps toward GR-like rotation

1. ~~Full baseline across cases~~ done (see table).
2. ~~Over-unfold veto~~ done and shipped (`maxDeltaVelocityMetersPerSecond 120`,
   `testCirculationOverUnfoldVeto`).
3. ~~Threshold sweep~~ done; winner promoted. Re-run `tools/rotation_sweep.py` any time
   new cases are added to `data/rotation_cases/cases.json`.
4. ~~Warning domain filter~~ done (v3b): warnings keep only in-domain polygons.
5. ~~Miss diagnosis~~ done (`--diagnose` stage counts; Mayfield section above).
6. ~~Replay persistence + storm-relative trials~~ done, both rejected on numbers
   (see Failed experiments below). Live display keeps `persistCirculations`.
7. Dense LLSD shear grid as a first-class product (the screenshot wash), fed by
   spatially-consistent dealiasing. The harness scores it unchanged.
8. Multi-radar cases (KLOT/KLSX neighbors for Mayfield far polygons).

### Failed experiments (kept out of defaults — read before retrying)

- Same-sign couplets (ssb=1.8): Moore CSI 0.108 → 0.062, +48 dets, zero new hits.
- Single-tilt TVS: Mayfield SPC 0.147 → 0.109, warn CSI flat. Opt-in only.
- Depth 1500 m + align 8 km: Mayfield CSI 0.158 → 0.145 (FAR 0.50 → 0.74).
- Fixed min-gate 3.0: helps far-field (KHTX CSI 0.179) but floods Moore
  (63 → 134 dets, CSI 0.108 → 0.051). Range slope (5 − 0.02/km) keeps both.
- Track persistence in replay (`--persist`, 2-hits/8 km, plus a
  motion-compensated variant): collapses on subsampled volumes (10–15 min gaps
  exceed track life) — KHTX CSI 0.330 → 0.06–0.12, Mayfield POD 0.19 → 0.09.
- Fixed-vector storm-relative (`--storm-dir/--storm-speed`): Mayfield POD
  0.19 → 0.31 but FAR 0.50 → 0.86 (CSI down); Moore 103 → 339 dets (CSI 0.024).
  One vector per multi-storm window is too crude; per-storm motion is future work.

### GR2 shootout protocol (how "better than GR2" gets decided)

No GR2 output has been run on these volumes, so no superiority claim is made.
GR2 ships NWS-style TDA/MDA markers plus azimuthal-shear and NROT washes. Anyone
with GR2 settles it: export GR2 markers for the three `cases.json` windows as
`volume_time,volume_file,kind,lat,lon,delta_v_mps,...` CSV and score with
`tools/rotation_verify.py` using `--radar/--max-range-km 150 --radius-km 15
--lead-min -10 --lead-max 45`. Numbers to beat (domain-filtered, v3 defaults):
Moore POD 0.875 / CSI 0.067 (8 events), Mayfield POD 0.19 / CSI 0.158
(32 events), Hackleburg POD 0.47 / CSI 0.330 (66 events). Published NWS TDA
skill sits in the same band — matching it with an open pipeline is the standing
goal, not a victory lap.
