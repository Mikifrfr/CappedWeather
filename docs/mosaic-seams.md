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
