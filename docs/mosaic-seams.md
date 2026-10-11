# Nearest-radar mosaic: seam measurements

Question: can two synced Level II volumes composite at matched tilts
(max or nearest), or do beam-geometry seams forbid it?

Tool: `tools/mosaic_probe.cpp`. Same decode plus geometry as the live
canvas (`decodeLevel2Archive`, `rangeAndBearing`, `gateIndexForSlantRange`,
`nativeValue`), fixed geographic grid, no network. Self-check (volume vs
itself) reads exact zeros, so the sampler adds no bias of its own.

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

Acquisition footnote: the probe needed offline site coordinates, and the
fallback map only carried 12 radars (KPAH/KVWX missing: "site not in
catalog"). Eight outbreak-belt sites were added from the live
`api.weather.gov/radar/stations` record with a hermetic core test, so
offline tools work past Moore without a network round.
