# Echo Tops, VIL, CREF, CAPPI, and VAD derived products

CappedWeather derives 2D products from each completed Level II volume plus a per-tilt wind fit from velocity. They are intentionally volume-derived approximations, not byte-for-byte reproductions of proprietary GR or NEXRAD RPG products.

## Echo top (18 dBZ)

For every gate in the lowest distinct reflectivity sweep, CappedWeather samples the nearest ray at the same azimuth and horizontal ground range in each higher sweep. Beam height uses the existing standard 4/3-earth geometry and includes the radar antenna elevation, so output is in **thousands of feet MSL (`kft MSL`)**.

The echo top is the highest 18 dBZ crossing in the sampled vertical column. When a sample at or above 18 dBZ is followed by a weaker sample, the crossing height is linearly interpolated between the two beam centers. If the highest available sweep is still at or above 18 dBZ, the displayed height is a lower bound limited by the scan top.

## Composite reflectivity (CREF)

For every gate in the lowest distinct reflectivity sweep, CREF is the
maximum reflectivity sampled across all tilts at the same azimuth and
horizontal ground range (same sampling as Echo Tops/VIL, no adjacency
rule: every sample is a real measurement). Encoded in dBZ with the base
reflectivity packing, so palettes and the cursor probe agree with REF
gate for gate. Reads hotter than any single tilt by construction.

## CAPPI (constant-altitude PPI)

Reflectivity linearly interpolated in dBZ between the adjacent-tilt
pair bracketing the selected altitude (MSL, 2/3/4/5/7 km in the
product panel; the altitude is a global setting, so every panel and
every stored frame agrees). Below the lowest sample the column reads
the lowest tilt (standard clamp); above the highest sample the gate is
missing, never an invented top. Non-adjacent tilts never interpolate,
same no-bridging rule as VIL. Changing the altitude re-derives resident
histories in the background; spilled frames re-derive lazily when
scrubbed to (each frame is tagged with its derivation altitude, so a
stale height can never display silently).

## Vertically integrated liquid (VIL)

VIL uses the standard approximation

```text
VIL = 3.44e-6 * integral(Z^(4/7) dh)  [kg/m2]
```

where `Z = 10^(dBZ/10)` and `h` is height in metres. For each adjacent valid pair, CappedWeather averages the two linear-Z values, caps that layer average at **56 dBZ**, applies the `4/7` power, and multiplies by the beam-centre height difference. A column requires at least two distinct tilts and at least one sample of 5 dBZ or greater.

## VAD wind profile (per tilt)

Each tilt carrying velocity moments gets a layer-mean horizontal wind from a
least-squares sine fit over dealiased gates:

```text
Vr(az) = a + b*sin(az) + c*cos(az)
speed = hypot(b, c) / cos(elevation), direction = atan2(b, c) (reported FROM)
```

The fit runs on Dealiased velocity at the panel/per-ray Nyquist, so folded
C-band OPERA scans do not manufacture winds. Every tilt reports gate count,
azimuth coverage, RMSE, median sample height (MSL), and a quality flag:
Good needs 200+ gates, a 270+ degree circle, and RMSE at most 3 m/s;
Marginal needs 60+ gates, 180+ degrees, RMSE at most 6. Solved-but-worse
fits show as Poor with their RMSE; singular geometry, fewer than 25 gates,
or under 90 degrees of coverage show as Invalid. Poor and Invalid rows stay
visible in the scan diagnostics pop-out so a bad fit never reads as calm
air. Guidance only, never warnings. Core contract tests fit a known wind on
a synthetic two-tilt volume (speed, FROM direction, tilt height separation)
plus singular, all-missing, and infinite-gate fail-closed cases.

## Bunkers storm motion + SRH (from the VAD rows)

Storm motion follows Bunkers et al. 2000 (ID method): gateCount-weighted
0-6 km AGL mean wind from Good/Marginal VAD rows, deviated 7.5 m/s
perpendicular-right of the deep-layer shear (upper 4-6 km mean minus lower
0-2 km mean). Shear under 5 m/s means no split: motion equals the mean wind
with a weak-shear flag. Both halves plus 4+ rows read Good, both halves
with 2-3 rows Marginal, anything less Invalid.

SRH integrates the same rows bottom-to-top per layer:

```text
SRH = Σ [(u_{i+1}-cx)(v_i-cy) - (u_i-cx)(v_{i+1}-cy)]
```

Storm motion is the right-mover when the shear splits, the mean wind when
weak. 0-1 km needs one pair spanning 500+ m; 0-3 km needs 2+ pairs spanning
1500+ m, so a shallow two-row profile reads "0-1 only" instead of a
deep-layer number. No surface interpolation: the lowest VAD row anchors the
bottom, and under-sampled boundary layers read low. Both print in the scan
pop-out summary with their quality flags. Core tests use closed-form
hodographs (uniform flow = zero SRH; linear shear with an off-line mover =
18.75 / 84.38 m²/s²) plus single-row, no-motion, and shallow-profile cases.

## VAD live-volume check (KTLX, 2026-10-09)
First real-data read, via `rotation_replay --vad` on three consecutive
Archive-II volumes (16:37, 16:44, 16:51Z, ~6 MB each, VCP212-style splits).
No tuning: default Nyquist, same dealiased velocity the dialog fits.

- Tilts 7-12 (1.7-6.4 deg): FROM 177-202 deg at 4.4-6.3 m/s, RMSE 3-4,
  Marginal/Good, nearly identical across all three volumes. A light
  south-southwesterly flow, temporally stable over 14 minutes.
- Tilts 2/4/6 (0.5-1.3 deg): correctly Poor, RMSE 5-17 with scattered
  directions. Low tilts mix ground clutter, weak echo, and folded storm
  gates; the quality flag withholds trust exactly where it should.
- Same-hour METARs (KOUN 140/06, KOKC 190/08) show the same southerly
  regime within 1-2 m/s: consistency context, not a calibration.
- Limits: one light-wind October day only; reported heights are sample
  medians near 1 km, not full-column soundings; convective-day and
  strong-shear cases are still unmeasured. Re-run with
  `rotation_replay <dir> --lat 35.333 --lon -97.278 --elev 371 --vad --csv`.

## Sampling and quality behavior

- At least two distinct reflectivity elevation angles are required. Repeated supplemental scans at effectively the same angle are deduplicated so they cannot inflate VIL.
- The lowest reflectivity sweep supplies the output azimuth/range grid. Higher sweeps are sampled at the same horizontal ground range using their own gate spacing and first-gate metadata.
- Missing, below-threshold, range-folded, out-of-range, and badly separated azimuth samples are skipped. VIL never bridges a missing intermediate tilt; that layer is omitted rather than inventing liquid water across an unconstrained gap.
- Products are generated when a complete or recovered volume finishes assembling. They update on each newly published volume. Progressive partial-ray snapshots (500 rays, then every 4000) paint base echo only. ET/VIL are never derived from partials.
- Partial volumes can produce holes or underestimated VIL where upper tilts are unavailable. Echo Tops without an adjacent weaker sample are retained as scan-limited lower bounds. The existing volume-quality status remains visible alongside the product. Note the echo-seam fix is rendering-only (same-color 1 px gate outlines + neighbor-spacing azimuth tiling): it closes 1 px background corduroy but never fills true data holes.

## Architecture

- Core implementation: `src/core/derived_products.{hpp,cpp}` (reflectivity family)
- VAD wind fit: `src/core/vad.{hpp,cpp}` (velocity family)
- Product descriptors: `src/core/product.{hpp,cpp}`
- Materialization point: `MainWindow::volumeReady()`
- Performance: elevation/range gate lookups are cached per sweep layout and the completed-volume pass runs on the global thread pool before publication, never on the GUI thread.
- Rendering/probing: existing `RadarMapCanvas` gate pipeline
- Core contract tests: threshold interpolation, linear-Z VIL integration, one-sample lower bounds, negative first-gate ranges, gate-center/index boundaries, and insufficient-volume rejection
