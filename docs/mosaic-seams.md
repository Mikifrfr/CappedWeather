# Nearest-radar mosaic: seam measurements

Question: can two synced Level II volumes composite at matched tilts
(max or nearest), or do beam-geometry seams forbid it?

Tool: `tools/mosaic_probe.cpp`. Same decode plus geometry as the live
canvas (`decodeLevel2Archive`, `rangeAndBearing`, `gateIndexForSlantRange`,
`nativeValue`), fixed geographic grid, no network. Self-check (volume vs
itself) reads exact zeros, so the sampler adds no bias of its own.

Verdict (2026-10-20): still a probe. Matched tilt, CAPPI matching and clock
lag search are closed; the last avenue, per-gate / resolution-aware blending,
was measured and shaves 3.7-27% off the point floor without changing the
decision. The floor is not resolution: it is coherent structure that survives
every averaging scale. See the last two sections.

## Moore 2013-05-20: KTLX 20:12:29Z + KINX 20:13:33Z (64 s apart)

Box 4x4 deg at (35.75, -96.42), 0.02 deg step, 0.45 deg tilts, echo at 10 dBZ:

- cells 7384, KTLX echo 1130, KINX echo 4109, either 4424, both 815
- coverage gain vs KTLX alone: +291.5% echo cells (mostly KINX-only range)
- both-echo delta: bias -3.58 dB, RMSE 10.62, p50 7.0, p90 17.0, 59.3% over 5 dB
- beam MSL mismatch where both see echo: mean -4850 m, p50 4765, p90 6124
- seam by beam mismatch: <=2 km n=1, <=4 km n=72 (bias -6.95, RMSE 11.32),
  over 4 km n=742

The headline storm sits ~30 km from KTLX (beam ~0.6 km MSL) and ~190 km
from KINX (beam ~4 km MSL): the two radars sample different altitudes, and
even the 72 beam-matched cells disagree at RMSE 11 dB (64 s time offset
moves the storm ~1.5 km, comparable to the grid step, on top of resolution
and partial-beam differences).

## Second pair excluded: sector-scan trap (KTLX 20:20:58Z + KINX 20:21:24Z)

The tool's tilt QC line caught it before the numbers could mislead:

- KTLX 0.46 deg: 208 rays, 1.5% echo, only 14/72 five-degree bins covered
- KINX 0.45 deg: 1407 rays, 6.9% echo, 72/72 bins covered (first pair: KTLX
  261 rays at 28/72 bins, so KTLX was already sector-scanning at 20:12 and
  narrowed further by 20:20)

KTLX sector-scanned the storm while KINX ran full surveillance at super-res
density. A same-tilt composite would silently mosaic a 70-degree wedge
against full coverage (both=35 cells). Excluded from the decision; the
lesson stays in the tool: rays, echo fraction, and bin coverage print on
every run, so degenerate pairs fail loudly instead of scoring quietly.

## Decision (2026-10-10)

Naive same-tilt max/nearest compositing is not trustworthy: it mixes
altitudes and misregisters by design. A mosaic earns trust only with
lowest-beam-wins or CAPPI-level matching plus sub-volume time sync, then a
second scored case. Until then the mosaic stays a probe, not a layer.

## CAPPI-matched probe (2026-10-11, same Moore pair)

`--cappi HMSL` samples each radar's tilt closest to HMSL MSL per cell
(identical azimuth/gate/QC rules as the matched path) and scores the same
seam table. Same volumes, box, step and threshold as the baseline above:

- baseline rerun (`--elev 0.5`): both 857, bias -3.14, RMSE 10.71, p50 7.0,
  p90 17.0, 60.1% over 5 dB (reproduces the 10-10 headline metrics: 815,
  -3.58, 10.62, 7.0, 17.0, 59.3%)
- CAPPI 1500 m: both 5396, bias -2.05, RMSE 9.67, p50 5.0, p90 16.0,
  48.2% over 5 dB; beam-matched (<=1 km) subset RMSE 5.13 (n=1311)
- CAPPI 2500 m: both 5470, bias -1.92, RMSE 9.67, p50 5.0, p90 16.0,
  47.6% over 5 dB; <=1 km subset RMSE 6.13 (n=1839)

Altitude matching cuts overall RMSE 10.71 to 9.67 with 6.3x more
comparable cells, and beam-matched cells reach 5-6 dB. The residual is
still time sync (64 s moves the storm ~1.5 km against a 0.02 deg grid)
plus resolution and partial-beam differences. Caveat: the both-echo
populations differ (CAPPI sees storm-top echo invisible at 0.5 deg), so
the RMSE delta mixes better matching with a different sample, not a
pure paired comparison. Still research-only: needs the second scored
case from the decision above before any layer.

## SAILS-duplicate caution (found 2026-10-11)

The same volumes match different 0.5 deg sweeps by target: `--elev 0.45`
lands KTLX 0.44 (256 rays, sector SAILS cut) and KINX 0.40 (152 rays),
while `--elev 0.5` lands KTLX 0.45 (261 rays) and KINX 0.45 (1220 rays,
full surveillance). The volumes carry duplicate near-0.5 deg sweeps and
the lowest-elevation tie-break picks whichever duplicate sorts first, so
matched-tilt numbers are sensitive to the target string, not just the
weather. CAPPI mode is immune (it samples every tilt). A pristine-tree
rerun confirmed the refactor changed nothing: stashed code prints
bit-identical baseline numbers.

## Second case: Mayfield 2021-12-11 KPAH + KVWX (91 s apart)

Independent outbreak, independent radars. Public archive
(`unidata-nexrad-level2`, the registry-documented successor of the
deprecated `noaa-nexrad-level2` bucket; the old bucket now 403s even
anonymous object GETs):

- `https://unidata-nexrad-level2.s3.amazonaws.com/2021/12/11/KPAH/KPAH20211211_010321_V06`
  (9,050,376 bytes, sha256 `CA81D61E…98675`)
- `https://unidata-nexrad-level2.s3.amazonaws.com/2021/12/11/KVWX/KVWX20211211_010452_V06`
  (10,117,898 bytes, sha256 `D5DDAA8A…BDF4`)
- Local filenames kept verbatim under gitignored `data/mayfield_replay/`.
- QC: KPAH 0.45 deg 169 rays 24/72 bins (sector/SAILS), KVWX 0.45 deg
  2778 rays 72/72 bins (full surveillance). Same box/step/threshold as
  Moore (4x4 deg at 36.9,-88.3, 0.02 step, 10 dBZ).

Results:

- baseline: both 2487, bias -0.18, RMSE 8.85, p50 5.5, p90 14.5, 51.5%
  over 5 dB; beam mismatch mean +2370 m
- CAPPI 1500 m: both 9907, RMSE 7.46; <=1 km subset RMSE 5.10 (n=2993)
- CAPPI 2500 m: both 10062, RMSE 7.51; <=1 km subset RMSE 5.63 (n=3444)

Same-population paired verdict (identical cells, both methods):

- CAPPI 1500 m: n=2320, base RMSE 8.86 vs CAPPI 8.48, win rate 33.6%
- CAPPI 2500 m: n=2174, base RMSE 8.84 vs CAPPI 9.15 (worse), win
  rate 41.8%
- Moore CAPPI 1500 m: n=832, base 10.78 vs CAPPI 10.44, win rate 20.1%

Reading: altitude matching gives a small pure gain at 1500 m (4% paired
RMSE both events) and can lose at 2500 m; most of the headline RMSE drop
comes from a larger, different sample, and beam-matched cells agree at
5-6 dB either way. The dominant residual is time sync (91 s here moves
the storm ~1.5-2 km against a 0.02 deg grid), not altitude selection.
This bounds the claim instead of inflating it: CAPPI matching alone does
not fix compositing, and the decision above (time sync plus second case
before any layer) stands, now with two scored cases behind it.

## Advection lag search (2026-10-11, `--advect`, both cases)

If the residual were volume time separation, shifting radar B should
minimize paired RMSE near storm-motion x separation (~1-2.5 km NE both
days). It does not:

- Mayfield CAPPI 1500 m: best (0E,-1N) RMSE 8.12 (n=2099) vs 8.48
  unshifted; surface shallow near center, rising past +/-3 km
- Moore CAPPI 1500 m: best (+3E,+3N) RMSE 9.81 (n=823) vs 10.44, an
  isolated dip surrounded by 10.5+ (noise fit, not a coherent minimum)

Uniform bulk advection recovers at most ~0.6 dB with no physically
coherent minimum on either case, so time sync is not the expected fix
either: the ~8-10 dB floor is resolution, partial-beam and microphysical
difference, not clock. A mosaic layer would need per-gate weighting or
resolution-aware blending to move that floor, which is new research, not
a tuning step. The probe keeps all three modes for re-scoring.

(2026-10-20: "resolution" was the wrong half of that list. Footprint
matching removes at most 7% of the paired variance on Moore and 23% on
Mayfield; partial beam, calibration and microphysics are the rest. See the
per-gate section below.)

Acquisition footnote: the probe needed offline site coordinates, and the
fallback map only carried 12 radars (KPAH/KVWX missing: "site not in
catalog"). Eight outbreak-belt sites were added from the live
`api.weather.gov/radar/stations` record with a hermetic core test, so
offline tools work past Moore without a network round.

## Per-gate / footprint blending (2026-10-20, both cases)

The last avenue this record left open: can per-gate or resolution-aware
blending beat the point floor at all? Same decode, same geometry, same grid.

Method (`--footprint own|common`, `--footprint-scale N`): instead of snapping
to the one gate nearest the cell, average every gate whose beam volume
overlaps it - cross-track half-width 0.5 x ray spacing x range, along-track
half-width 0.5 x gate spacing - inside the tilt the point CAPPI rule already
picked. A footprint number therefore differs from its paired point baseline
only by the averaging window. `own` uses each radar's own footprint; `common`
uses the coarser of the two at that cell, the matched-scale comparison. `N`
multiplies both half-widths: 1 is the instrument sampling footprint (0.5 deg
x 250 m super-res), 2-3 approximates the physical 0.925 deg beam. Censored
gates (0/1 = missing/below detection) are counted and never averaged, so no
value is invented and the valid share rides in every table. No interpolated
fill, no display smoothing: this is a beam-volume average, and the second
table is what keeps it honest.

Self-check: the same volume on both sides reads exact zeros through all three
paths (point n=795, own 2x n=713, common 4x n=753; bias +0.00, RMSE 0.00), so
the estimator adds no bias of its own.

Paired same-cell, 1500 m CAPPI, 0.02 deg grid, 10 dBZ. Each row is scored on
one set of identical cells, so the point/footprint change cannot be flattered
by a population shift (points drift a little between rows because a larger
footprint loses edge cells):

Moore 2013, paired n=4444 (1x) / 4248 (2x) / 4300 (4x):
- 1x, mean footprint +-0.74 km KTLX / +-0.83 km KINX cross-track: point
  8.80 dB, own 8.72 (-1.0%), common 8.48 (-3.7%), common wins 42.9% of cells
- 2x: point 8.89, own 8.20 (-7.8%), common 8.00 (-10.1%), wins 58.3%
- 4x: point 8.95, own 7.83 (-12.6%), common 7.50 (-16.2%), wins 60.9%

Mayfield 2021, paired n=3778 (1x) / 3760 (2x) / 4018 (4x):
- 1x, +-0.76 km KPAH / +-0.66 km KVWX: point 7.44, own 6.69 (-10.1%), common
  6.53 (-12.1%), wins 49.2%
- 2x: point 7.56, own 6.36 (-15.9%), common 6.02 (-20.4%), wins 60.0%
- 4x: point 7.63, own 6.02 (-21.0%), common 5.56 (-27.2%), wins 61.7%

Censoring control, restricted to cells where both footprints are >=90% valid
(a partly-censored mean cannot flatter the result): Moore 8.66 -> 8.35
(-3.6%) at 1x, 8.43 -> 7.35 (-12.8%) at 4x; Mayfield 7.43 -> 6.58 (-11.4%)
at 1x, 7.79 -> 5.84 (-25.0%) at 4x. Same direction, slightly smaller at 1x.
Gates summed were 88.9%/90.5% valid (Moore KTLX/KINX, 1x) and 77.2%/88.0%
(Mayfield, 1x), so the censoring was not marginal.

## Why it stops there: the delta is coherent, not sub-footprint noise

`--blocks` averages the paired delta over 1/2/4/8/16-cell blocks (2.2-35 km),
splits its variance into within-block and between-block parts, and compares
the between part with what spatially white noise carrying the measured
within-block variance would leave (white_pred). Ratio ~1 = white; ratio >> 1
= neighbouring cells disagree the same way.

Moore, point CAPPI: block-mean RMSE 8.80 / 7.18 / 5.75 / 4.71 / 3.68 dB at
1/2/4/8/16 cells; between/white 6.6 / 7.5 / 11.4 / 16.1.
Mayfield, point CAPPI: 7.53 / 5.79 / 4.31 / 3.10 / 2.37 dB; between/white
4.9 / 4.7 / 5.4 / 8.2.

Three readings:

- Footprint matching removes at most 7% (Moore) / 23% (Mayfield) of the paired
  variance at the instrument footprint, and 30% / 47% at 4x. That is the whole
  resolution share, and it is the smaller half of the problem.
- The delta is 5-16x more spatially organised than white noise at every scale
  tried, so averaging cannot remove it: it is the same disagreement in
  neighbouring cells, not independent gate noise.
- 3.68 dB (Moore) and 2.37 dB (Mayfield) survive 35 km blocks, where both
  radars' footprints are ~20x finer than the block. Resolution cannot explain
  that residual. Beam altitude can: beam MSL mismatch is p50 2.3 km on both
  cases even after CAPPI matching, and the seam table follows it (Moore
  <=1000 m n=1296 rmse 5.09 vs >4 km n=1569 rmse 11.39). Calibration and the
  64/91 s of microphysical evolution are the rest; the advection search
  already ruled out bulk lag.

Where the beams already match, footprint matching does help, consistently:
Moore <=1000 m RMS 5.09 (point) -> 4.90 (1x) -> 4.30 (2x) -> 3.99 (4x),
Mayfield 5.92 -> 5.72 -> 4.84 -> 4.45. It helps most where a mosaic needs it
least.

## Verdict (2026-10-20): no layer, and the floor is not resolution

- Best measured paired-cell improvement over the CAPPI point baseline, on both
  cases: -16.2% (Moore) and -27.2% (Mayfield) at a 4x common footprint, win
  rates 60.9% and 61.7%; -3.7% and -12.1% at the real instrument footprint.
- What that leaves is 7.50 / 5.56 dB at 4x and 8.48 / 6.53 dB at 1x: the same
  order as the point floor, not a step change. A seam between two radars would
  still jump by several dB.
- 2.4-3.7 dB of the difference is present at every scale up to 35 km and is
  5-16x more organised than noise. Per-gate blending along the resolution axis
  cannot reach it; the controlling terms are beam altitude, calibration, and
  time/microphysics.
- The 2026-10-11 reading that the floor was "resolution, partial-beam and
  microphysical difference" was half right and mostly wrong: partial beam is
  real and dominant, sub-footprint representativeness is the small term.

Decision: the mosaic stays a probe. A layer needs lowest-beam-wins (or a true
lowest-beam CAPPI) plus inter-radar bias correction; neither is in this card's
scope. `tools/mosaic_probe.cpp` keeps all five modes (matched tilt, CAPPI,
advection, footprint, blocks) so a future attempt is rescored on the same two
cases.

## Provenance: what reproduces and what does not (2026-10-20)

- Moore reproduces the 2026-10-10 headline to the digit on the current archive
  objects (cells 7384, both 815, bias -3.58, RMSE 10.62, p50 7.0, p90 17.0,
  59.3%), so the tool is validated against this record.
- Mayfield does not. The files named above (sha256 re-verified, byte-identical
  to the recorded sizes) give baseline both 1890, bias +0.06, RMSE 8.44 now,
  where the 2026-10-11 section records 2487 / -0.18 / 8.85. No plausible box
  shift reproduces 2487 (five centres tried: 1823-1968). The pre-change binary
  (commit ce4259d) prints the same 1890 / 8.44 on these files, so it is not
  this card's refactor. Treat the 2026-10-11 Mayfield absolutes as
  unreproducible.
- The 2026-10-11 CAPPI absolutes are in the same position: their recorded
  baseline rerun scores 857 where the current objects score 815, and their
  Moore CAPPI both-echo 5396 / RMSE 9.67 runs 11% above this run's 4784 / 8.80
  on the same box. Different volume files were in play. Every number in the
  new sections is same-run, so the verdict does not depend on either.

The live acquisition for this card (2026-10-20): the 2013 objects are served
gzip-compressed (`KTLX20130520_201229_V06.gz` 9,293,467 B, sha256
e1160f97...05f100; `KINX20130520_201333_V06.gz` 8,035,019 B, sha256
5cc388fd...2c330b) and gunzip cleanly to 44.9 / 45.0 MB volumes; the 2021
objects are plain. Both radars' site coordinates and antenna MSL came from the
offline catalog, no network at sample time.
