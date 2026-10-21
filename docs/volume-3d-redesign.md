# 3D volume rendering: redo

Status: design. Supersedes the render-path sections of `gpu-volume-rendering.md`;
the `VolumeBrick` data layer from that doc is kept unchanged.

Target look: the aguacerowx 3D NEXRAD view / CM1-style storm rendering — a lit,
semi-transparent volume you can orbit, with the storm reading as a solid object
rather than colored fog.

## Why the current renderer cannot reach that look

Measured from `paintVolume3d` / `rebuildVolumeRender` in `radar_map_canvas.cpp`.
These are not taste complaints; each one caps the result on its own.

1. **Rays are truncated at 104 km.** `stepMeters` is the smallest voxel dimension
   (208 m vertical) × 1.25 = 260 m, and the march is capped at 400 steps. Box
   diagonal is 425 km and needs 1630 steps. Everything past 104 km along the ray
   is never sampled. A second radar, an anvil back edge, a distant line — all
   silently absent.
2. **Nearest-neighbour sampling.** `sampleBrickNearest` snaps every sample to a
   voxel centre. No trilinear, so the field is a stack of cubes: visible facets,
   hard steps between tilts, no continuity when orbiting.
3. **No lighting at all.** Colour comes from `evaluateTransfer` and alpha from
   intensity; nothing reads a density gradient or a light vector. A volume
   without gradient shading has no shape. This is the single biggest reason it
   reads as fog — CM1 imagery is dominated by directional shading, not by colour.
4. **The transfer function has no opacity dimension.** Every stop is built with
   `a = 1.0F`; visibility comes from a hard `intensity >= 0.25` cutoff plus a
   smoothstep over `(intensity-0.25)/0.40`. Low reflectivity is either fully
   invisible or abruptly visible, with no per-value translucency, so edges are
   binary and the interior is flat.
5. **Voxels are too coarse for a storm.** ±150 km at 256² = 1.17 km per voxel.
   A 40 km storm occupies 34 voxels across; its structure is smaller than one
   voxel. Vertical is 208 m, giving 5.6:1 anisotropy.
6. **Half resolution, upscaled.** `divisor = 3` while interacting, 2 at idle, then
   drawn with `SmoothPixmapTransform`. The marshal happens at a fraction of the
   panel and is stretched.
7. **No empty-space skipping.** Every ray walks the full truncated length, so the
   work is spent on hundreds of empty samples instead of the storm.

Fixing 1, 3 and 5 changes the look more than any shader would.

## What the target look actually is, decomposed

The reference image is not one effect; it is five, in order of contribution:

| Contribution | What it is | Where it lives |
|---|---|---|
| Directional shading | Gradient of density dotted with a light vector, plus ambient | shading, not data |
| Per-value opacity | Low dBZ fully transparent, mid translucent, core opaque | transfer function |
| Trilinear continuity | Smooth sampling so structure has surfaces | sampling |
| Enough resolution | Storm occupies hundreds of cells, not tens | brick ROI |
| Depth cues | Ground plane, height box, vertical scale | camera + chrome |

All five are renderer-side. None needs a new data source, which is why this is a
redo of the render path rather than a new product.

## Decision: two phases, cheap look first

**Phase 1 — CPU look pass (hours).** Fix defects 1, 2, 3, 4 and 5 on the existing
march. No GPU work. Deliverable: the view already looks like a storm volume.

**Phase 2 — GPU raymarch (days).** `QRhiWidget` (verified present in Qt 6.8.3),
3D texture, full-resolution per-pixel march, occupancy skip, display modes.

Why this order, against the obvious objection that the GPU is the real answer:

- The look parameters — opacity ramp, light direction, ambient level, gamma —
  are tuned by eye against real volumes. Tuning them inside a shader while also
  debugging RHI, texture upload and an offscreen render target means every
  visual result has three possible causes. Phase 1 tunes them where a bad frame
  costs a rebuild, not a GPU debug.
- Both phases must share one transfer function anyway (the existing design doc
  requires the CPU twin so probe and cross-section readouts agree). Whatever
  Phase 1 sets becomes the shader's constants, so it is not throwaway work.
- Phase 1 fixes two outright bugs (truncation, nearest sampling). The GPU path
  would inherit both if it were built on top of them.

The 7800 XT has the headroom for Phase 2; it is not the constraint. Verification
time is.

## Phase 1 specification

Data (`src/core/volume_brick.*`, additive, existing tests must stay green):

- `buildVolumeBrick(chunk, product, dims, roi, incomplete)` where `roi` is an ENU
  box. Default remains the full ±150 km box, so no caller breaks. A storm ROI of
  40 × 40 × 18 km at 256 × 256 × 120 gives ~156 m voxels isotropically.
- `sampleBrickTrilinear(brick, x, y, z)` returning value and a validity flag.
  Validity is separable from value: a voxel with `present == 0` is *unknown*, not
  zero, and must not be interpolated as zero. This is the honesty rule for the
  whole feature — smoothing across a data hole must not manufacture echo.
- `brickGradient(brick, x, y, z)` central differences on the trilinear field,
  returning a unit normal where the gradient is non-degenerate.

Render (`src/app/radar_map_canvas.cpp`):

- Step per axis from the brick's own extents, not the smallest dimension:
  `step = 0.75 * min(dx, dy, dz)` is still too fine for a 425 km box; instead
  step by `max(dx, dy, dz) * 0.5` with trilinear, and cap steps by geometry
  (`diagonal / step + 2`), never by a magic constant.
- Front-to-back with per-sample opacity from the transfer function, early-out at
  0.98 coverage, and a floor on `present` so unknown voxels are skipped.
- Lighting per sample: `shade = ambient + (1-ambient) * max(0, dot(normal, light))`
  with `light` a fixed unit vector in ENU and `ambient` ≈ 0.25. Modulate colour
  only; alpha stays a function of value.
- Opacity ramp as data, not code: extend `RgbaStop` usage so stops carry alpha,
  built from a per-product ramp (reflectivity: 0 below 10 dBZ, rising to ~0.35 at
  40, ~0.85 at 55+). One table per product, in core, unit-tested.
- Resolution: keep the divisor for interaction, but run one full-resolution
  refine pass when the camera is idle for 250 ms, replacing the current
  "half res forever" behaviour.

Acceptance for Phase 1, all measured:

- Ray reach equals box diagonal on a synthetic brick (a marker voxel at the far
  corner must appear). Today it cannot.
- A hole in the field survives as a hole: a synthetic brick with a hollow centre
  renders a visible void, not a filled blob.
- Gradient normal is unit length or reported degenerate on a constant field.
- Frame time at 900 × 700 on this machine, half res: recorded, not guessed.
- Existing `VolumeBrick` tests unchanged and green.

## Phase 1.1 — vertical fill between sweeps (implemented)

The Phase 1 march was still fourteen tilted shells, because the splat wrote every
gate into exactly one voxel and left the space between adjacent sweeps empty.

Measured on KTLX 2013-05-20 20:12Z REF, full-range brick (300 km, 256 x 256 x 128,
156 m levels): of the 25,724 columns that had more than one measured level,
**0.780 of the vertical span between the lowest and highest measured voxel was
empty**. 251,336 voxels were observed; everything between the sweeps was nothing.

`buildVolumeBrick` now runs a vertical pass after the max-wins splat:

- For each `(ix, iy)` column, walk z and bridge the empty levels between two
  measured voxels, lerping the quantized value so the column transitions rather
  than stepping.
- Only measured voxels bound a run. No column grows a top or a bottom no gate
  supports, and the fill is bounded by its two ends, so it can never invent a
  stronger return than the samples it sits between.
- Only sweeps that differ may be bridged. A chunk with `elevationNumber` uses it
  as the sweep id (Message 31 rejects rays without one, ODIM numbers its
  sweeps), so encoder jitter inside one tilt cannot look like a second view; a
  chunk with none falls back to a 0.1-degree bucket. One tilt's beam climbing
  across a column is one view, not two, and is never filled with itself.
- The max-wins rule for measured gates is unchanged.

Reconstructed voxels are marked, not blended in silently:

- `VolumeBrick::interpolated` is a parallel byte mask, set exactly on filled
  voxels and clear on measured ones; `present && !interpolated` is the observed
  set. `measuredVoxels`, `interpolatedVoxels` and `interpolatedTilts` carry the
  counts so a readout does not rescan the mask.
- The renderer samples filled voxels at full weight. Discounting their opacity
  would recreate the shell look this pass exists to remove; the honesty lives in
  the chip instead, which states the fill whenever there is one:
  `3D preview (CPU)  KTLX REF  1041 ms  - 17 tilts interpolated (78% reconstructed)`.

Same volume, same camera, same binary with the fill switched off and on
(`tools/volume3d_probe`, 1440x900):

| | fill off | fill on |
|---|---|---|
| occupied voxels | 251,336 | 1,132,399 |
| reconstructed share of occupied | 0 | 78% |
| empty share of each column's span | 0.780 | 0.00001 |
| lit_fraction | 0.0597 | 0.1343 |
| checksum | 326,463,133 | 436,387,229 |
| brick build | 166 ms | 230 ms |
| frame | 643 ms | 1041 ms |

The march costs 62% more because there is 4.5x more data to shade: gradient
shading (`brickSurfaceNormal`, seven trilinear fetches per lit sample) is 507 ms
of the 1041 ms frame. It lands on the 1048 ms budget this pass was measured
against, but the cost is real, and the fix is empty-space skipping on a column
occupancy mask plus the Phase 2 GPU march, not a coarser step. Repeat runs under
a loaded machine (62% CPU, another build running) scale the pair together:
835 ms off, 1325 ms on.

Tests: `testVolumeBrickFillsBetweenTilts` (a two-sweep chunk with a known gap
produces a continuous column, the mask is set exactly on the fill and clear on
both endpoints, values stay monotone between the endpoints, a third sweep in
another column adds nothing) and `testVolumeBrickDoesNotFillOneCone` (one sweep
whose own beam climbs ten levels inside one column still produces zero
interpolated voxels). Both were shown failing on the old path first: with the
fill disabled `core_tests` exits 1 at `core_tests.cpp:3960`; with the sweep guard
removed it exits 1 at `core_tests.cpp:4044`.

Screenshots: `docs/screenshots/volume3d-shells.png` (before, no fill) and
`docs/screenshots/volume3d-filled.png` (after).

## Phase 2 specification

- `VolumeCanvas : QRhiWidget`, own top-level widget inside the panel stack, not
  a QPainter draw. D3D11 first on Windows (least driver variance), with the RHI
  backend selection logged once at startup.
- Brick to `R8` 3D texture with linear filtering; `present` as a second channel
  or a packed bit so the shader can reject unknown samples.
- Full-screen triangle, ray reconstructed from inverse view-projection, slab test
  against the brick AABB in the shader, march with the same constants as Phase 1.
- 32³ occupancy texture built during the splat for empty-space skipping.
- Display modes as one uniform: `lit` (Phase 1 shading), `cloud` (higher opacity,
  softer gradient, no ambient floor), `max` (per-pixel maximum instead of
  front-to-back accumulate), `solid` (isosurface at a threshold, shaded with a
  stronger specular term).
- Fallback: if shader compile or texture upload fails, log once and drop that panel
  back to the Phase 1 CPU path. Never a black panel.

## Camera and chrome

Orbit around the site in local ENU, reusing the site lat/lon the 2D canvas already
holds. Ground grid + 50 km range rings + site marker + a height box in kft. A
vertical scale multiplier (1×–4×) because a 20 km top over a 300 km base is
visually flat; this is a presentation control, and the status line states when it
is not 1× so the view is never mistaken for true geometry.

## Risks

- Trilinear over a `present`-masked field can bleed echo across a hole boundary by
  half a voxel. Accepted for v1, and the hole test pins the behaviour; the honest
  alternative (sharpen at mask edges) costs more than it buys.
- Full-res CPU refine may be too slow to be worth it on 4K panels. If measured
  frame time exceeds 400 ms, drop the refine and say so rather than shipping a
  stall.
- Volume shadows / ambient occlusion are out of scope. They are what would push
  this past CM1 fidelity, and they are a Phase 3 question once the base look is
  right.
