# Echo Tops and VIL derived products

CappedWeather derives two 2D products from each completed Level II reflectivity volume. They are intentionally volume-derived approximations, not byte-for-byte reproductions of proprietary GR or NEXRAD RPG products.

## Echo top (18 dBZ)

For every gate in the lowest distinct reflectivity sweep, CappedWeather samples the nearest ray at the same azimuth and horizontal ground range in each higher sweep. Beam height uses the existing standard 4/3-earth geometry and includes the radar antenna elevation, so output is in **thousands of feet MSL (`kft MSL`)**.

The echo top is the highest 18 dBZ crossing in the sampled vertical column. When a sample at or above 18 dBZ is followed by a weaker sample, the crossing height is linearly interpolated between the two beam centers. If the highest available sweep is still at or above 18 dBZ, the displayed height is a lower bound limited by the scan top.

## Vertically integrated liquid (VIL)

VIL uses the standard approximation

```text
VIL = 3.44e-6 * integral(Z^(4/7) dh)  [kg/m2]
```

where `Z = 10^(dBZ/10)` and `h` is height in metres. For each adjacent valid pair, CappedWeather averages the two linear-Z values, caps that layer average at **56 dBZ**, applies the `4/7` power, and multiplies by the beam-centre height difference. A column requires at least two distinct tilts and at least one sample of 5 dBZ or greater.

## Sampling and quality behavior

- At least two distinct reflectivity elevation angles are required. Repeated supplemental scans at effectively the same angle are deduplicated so they cannot inflate VIL.
- The lowest reflectivity sweep supplies the output azimuth/range grid. Higher sweeps are sampled at the same horizontal ground range using their own gate spacing and first-gate metadata.
- Missing, below-threshold, range-folded, out-of-range, and badly separated azimuth samples are skipped. VIL never bridges a missing intermediate tilt; that layer is omitted rather than inventing liquid water across an unconstrained gap.
- Products are generated when a complete or recovered volume finishes assembling. They update on each newly published volume. Progressive partial-ray snapshots (500 rays, then every 4000) paint base echo only — ET/VIL are never derived from partials.
- Partial volumes can produce holes or underestimated VIL where upper tilts are unavailable. Echo Tops without an adjacent weaker sample are retained as scan-limited lower bounds. The existing volume-quality status remains visible alongside the product. Note the echo-seam fix is rendering-only (same-color 1 px gate outlines + neighbor-spacing azimuth tiling): it closes 1 px background corduroy but never fills true data holes.

## Architecture

- Core implementation: `src/core/derived_products.{hpp,cpp}`
- Product descriptors: `src/core/product.{hpp,cpp}`
- Materialization point: `MainWindow::volumeReady()`
- Performance: elevation/range gate lookups are cached per sweep layout and the completed-volume pass runs on the global thread pool before publication, never on the GUI thread.
- Rendering/probing: existing `RadarMapCanvas` gate pipeline
- Core contract tests: threshold interpolation, linear-Z VIL integration, one-sample lower bounds, negative first-gate ranges, gate-center/index boundaries, and insufficient-volume rejection
