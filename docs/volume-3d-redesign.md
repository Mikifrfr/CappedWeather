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
  `3D  KTLX REF  1041 ms  (CPU)  - 17 tilts interpolated (78% reconstructed)`.
  The `(CPU)` / `(Direct3D 11)` field names the raymarcher that drew the frame;
  before card t_51d7af1a the chip read `3D preview (CPU)` and could not tell a
  GPU frame from a CPU one.

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

`lit_fraction` in this table is the CPU panel grab **before** card t_cc6aa591
fixed the preview's premultiplied alpha. The fix raises such a grab by ~12%
relative (full-range box, same camera: 0.13734 -> 0.15426, see
`docs/gpu-volume-rendering.md`); the fill-off/fill-on comparison is unaffected
because both columns are pre-fix.

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

## Phase 1.2 — the brick box is the user's, not a guess (implemented)

The brick used to be the full +-150 km box, always. Automatic ROI selection was
attempted three times and failed three times, each time painting a
healthy-looking empty frame:

1. box centred on the radar site -> a storm 60 km out fell outside it;
2. box centred on the echoed centroid -> a scattered field averages to the gap
   between cells, so the box framed nothing;
3. box sized to the echo bounding box -> far-range speckle inflated it back to
   full range and the storm became a dot.

None of those reported an error; each reported a plausible frame time. The
lesson is not "a better heuristic" — it is that the box is the user's decision.
`Alt+left-drag` inside the 3D view now defines it.

### The rule

- **Drag on the ground plane.** Both drag corners are mapped onto z = 0 with the
  same camera the frame was marched with (same aspect, same focal), so the box
  lands where the pixel looks. A corner above the horizon has no ground point;
  it clamps to the coverage circle along its own heading rather than inventing a
  distance.
- **Square, and shown before release.** The brick has one half extent for x and
  y (`VolumeBrickDims::halfExtentMeters`), so the box that can be built around a
  rectangle is the square that covers it. The drag paints that square live
  (dashed wireframe), so the box on screen is the box built: nothing is expanded
  or moved after the mouse comes up.
- **Clamped, not rejected.** Minimum 8 km per side: a two-pixel drag builds an
  8 km box (31 m voxels at 256 columns) instead of a degenerate grid. The box is
  clamped to the radar's own coverage — the ground range of the farthest decoded
  gate (459 km on this KTLX volume) — by shrinking the half extent and pulling
  the centre in along its own bearing. A box centred outside coverage is moved
  in, never snapped onto the site.
- **Reset** via the 3D right-click menu ("Reset 3D box to full range"): the
  full +-150 km box and the default camera. The full-range box is the default and
  is never empty on a live volume. Nothing is persisted; this is a view action,
  not a setting.
- **An empty box says so.** If no gate lands inside the box the chip reads
  `- no gates in box` instead of showing a quiet frame that looks like calm
  weather. This is the exact failure mode the three automatic attempts had.

### Measured, KTLX 2013-05-20 20:12Z REF, 1440x900

`tools/volume3d_probe` gained `--box cx_km,cy_km,half_km | --box full` and
`--drag x0,y0,x1,y1` (panel pixels, the same entry point the mouse uses), plus
`box_center_km/brick_center_km/box_matches_brick`. Median of three alternating
runs on a quiet machine for the first two columns, one run each for the last two:

| | full range | 60 km box (dragged) | 8 km box (clamped) | empty 40 km box |
|---|---|---|---|---|
| brick half extent | 150 km | 29.998 km | 4 km | 20 km |
| voxel (x/y) | 1172 m | 234 m | 31 m | 156 m |
| occupied voxels | 1,132,399 | 1,594,302 | 4,623 | 0 |
| reconstructed share | 78% | 74% | 27% | - |
| lit_fraction | 0.1373 | 0.1653 | 0.0061 | 0.0127 (chrome only) |
| brick build | 285 ms | 310 ms | 312 ms | 242 ms |
| frame | 1,283 ms | 6,392 ms | 120 ms | 2,538 ms |

`lit_fraction` here is also a pre-t_cc6aa591 CPU panel grab (~12% low); the
full-range box reads 0.15426 after the alpha fix, and the relative comparison
across boxes is unaffected.

The same grid is used for every box (256 x 256 x 128), so the voxel size is the
whole point: a 60 km storm is 51 columns wide at full range and 256 columns wide
in a 60 km box — 5x finer across the storm, in both axes and in height.

Screenshot pairs: `docs/screenshots/volume3d-roi-full.png` (full range, the box
wireframe now marks what the frame covers), `volume3d-roi-storm.png` (the Moore
supercell in a 60 km box) and `volume3d-roi-empty.png` (a box with no gates, with
the chip saying so).

### What this exposes: a closer box is not a cheaper frame

The march step is `0.5 x max(voxel dimension)` and the step count is bounded by
the box diagonal, so a small box takes about the same number of steps per ray as
the full-range pancake (746 vs 727). What grows is how much of the screen the box
covers: at 420 km the 300 km x 20 km pancake intersects a thin horizontal band of
the frame; at 84 km the 60 km x 20 km box fills a large part of it. Same steps,
~5x the pixels, and the gradient shading is seven trilinear fetches per lit
sample. Result: **6.4 s per frame for the storm box against 1.3 s for the full
range**, and 2.5 s for a box with *no gates at all* (nothing stops the march
early). The 8 km box is fast only because it is a small spec on screen.

This is recorded, not solved here: the fix is empty-space skipping plus the GPU
march (`t_710dd647`), and a box view that looks this much better while costing
5x is exactly the case the GPU spike exists to prove. Until then the chip states
the frame time it actually took, so a 6 s box never reads as instant.

### Tests

`testVolume3dBoxDrag` (hermetic: every gate is built in memory) asserts, against
the brick the canvas actually publishes:

- a drag's two ground points map back onto the same pixels through the projector
  (two-way, within 2 px), so the screen mapping is not one-way;
- the built brick's centre and half extent equal the drag rectangle's in ENU
  metres, within one voxel (voxel = 2 x half / 256);
- the dragged brick is not empty;
- a two-pixel drag clamps to 8 km and the clamped box is what reaches the brick;
- `resetVolume3dBox()` returns the full-range box and the full-range brick after
  reset is not empty;
- `clampVolume3dBox` keeps a box out at 300 km inside a 100 km coverage circle.

Failing first: with the box removed from the brick dimensions (the pre-change
behaviour: always the default full-range `VolumeBrickDims`) `cappedweather_ui_tests`
exits 1 with `3D box drag failed: the dragged box never reached the brick`
(`testVolume3dBoxDrag`, `tests/ui_smoke_test.cpp`). Headless on the real volume,
the pre-change probe binary ignores `--box` entirely and reports the full-range
brick whatever box is asked for: `lit_fraction=0.134265 ... frame_ms=1347`,
1172 m voxels, 51 columns across the storm.

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
