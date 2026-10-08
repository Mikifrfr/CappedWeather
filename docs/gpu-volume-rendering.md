# GPU volume rendering of live Level II: design

Status: design, not scheduled. 2D stays the primary console; this adds one 3D view.

## Decisions (2026-10-08)

- Panel model: per-panel 2D/3D toggle. Any panel flips. Fill-rate guard: only
  panels visible on screen render, and resolution steps down automatically when
  more than one 3D panel is live (first 3D panel full, extras half res).
- Render widget: cross-API from the start (`QRhiWidget` spike first). The dev
  GPU here is a 7800 XT and users bring whatever they have (NVIDIA and more),
  so one backend per vendor API is out. RHI gives D3D/Vulkan/Metal/GL from one
  shader set. The CPU twin stays mandatory regardless.
- M1: core brick plus transfer plus tests plus CPU preview. Go.

## What v1 is

Any panel can flip between the 2D map and a 3D volume view of its own current
volume and product. The 3D view ray-marches reflectivity in a local box around
the radar (about 300 x 300 x 20 km). Drag orbits, wheel zooms, timeline scrub
swaps the volume. Reflectivity only in v1. Dual-pol tint, warning polygons in
3D, and basemap drape are explicitly v2.

## Data path (shared by GPU and CPU)

Today a volume arrives as `DecodedChunk`: polar rays with per-product encoded
fields plus scale/offset (`core/level2_decoder.hpp`), and the 2D canvas builds
`EchoGateGeometry` quads per volume/product/elevation. The 3D path adds one
parallel structure:

- `VolumeBrick`: Cartesian voxel brick built on a worker thread from the same
  `shared_ptr<const DecodedChunk>`. Fixed grid, e.g. 256 x 256 x 96, uint8
  normalized per product plus a nodata mask bit. Beam height from the existing
  4/3-earth helper (`standardBeamGeometry`), same function the derived
  products use, so ET/VIL and the brick agree on height.
- One brick per (volume, product). LRU cache of 4 bricks so timeline scrub
  back and forth does not rebuild. Rebuild cost target: under 300 ms on a
  worker thread for a 10-tilt volume (roughly 7M gates splatted into 6.3M
  voxels; splat is O(gates), not O(voxels)).
- Partial volumes (500-ray snapshots during load) upload what exists and flag
  the brick `incomplete`, same honesty rule as the 2D `loading` pill.
- Single-tilt OPERA volumes produce a flat brick; the view degrades to a
  tilted-slab render instead of failing. No faked thickness.

Memory: 256 x 256 x 96 uint8 is ~6 MB per product. Two products resident is
fine on iGPU; cap the LRU by bytes (e.g. 64 MB), not by count.

## Render path

### GPU (primary when available)

Any panel flips between 2D and 3D, showing its own current volume and product.
The 3D backend is a `QRhiWidget`-based view (Qt 6.8 ships `QRhiWidget` plus
the RHI headers, verified in the local Qt install), so one shader set covers
D3D, Vulkan, Metal, and GL across AMD, NVIDIA, and Intel. A `QOpenGLWidget`
remains the fallback only if the RHI spike hits a wall; it is not the plan.

Fill-rate guard for multiple live 3D panels: only visible panels render, and
resolution steps down when more than one 3D panel is live (first full, extras
half res with smooth upscale).

- Upload: brick as `GL_R8` 3D texture (or 2D array if 3D textures prove flaky
  on old Intel drivers), palette as 1D lookup texture generated from the
  existing `PaletteKind`/`colorForValue` path so 2D and 3D agree on color.
- Ray march in the fragment shader: front-to-back, early termination at
  ~98% opacity, empty-space skipping via a coarse occupancy texture (32^3,
  one byte per 8^3 block, built during the splat for free).
- Camera: perspective orbit around the site in local ENU. Reuse the site
  lat/lon the 2D canvas already holds; no new geo code.
- Overlays in v1: ground grid + range rings + site marker only. No warnings,
  no lightning, no basemap tiles inside the GL surface (tiles stay a 2D
  concern; v1 background is the Squall dark).
- Failure rule: shader compile or texture upload failure logs once and flips
  that panel to the CPU fallback automatically. Never a black panel with no
  explanation; the status line says which path is active.

### CPU fallback (same interface, always built)

`CpuVolumeView` renders the identical `VolumeBrick` with the identical camera
math into a `QImage`, tiled across `QThreadPool` like today's `rebuildOverlay`
worker. Differences are only quality/speed policy:

- Half resolution, then progressive refine while idle (same pattern as the
  2D partial snapshots: fast coarse, refine on pause).
- Same transfer function code path on CPU (shared `volumeTransfer()` in core,
  unit-tested), so GPU/CPU pixel-match within quantization.
- Auto-selected when GL < 3.3, on upload failure, or via View menu override
  persisted as `view/volumeRenderer` (`auto` / `gpu` / `cpu`).

The point of sharing brick + camera + transfer function: the fallback is a
render backend, not a second product. Any probe or cross-section readout
agrees regardless of backend.

## Integration points

- Timeline: `showFrame()` hands the 3D panel the same chunk the 2D panels
  get. Brick cache keyed by (radar, dataTime, product).
- Probe: clicking in 3D casts the ray and reports the first opaque sample as
  lat/lon/height/value through the existing pinned-card path (`PinnedInfoKind::Probe`).
- Cross-section: v2 reuses the brick for instant arbitrary cuts (free win once
  the brick exists; today XS resamples rays per frame).
- Settings: renderer override, brick resolution (Low/Med), opacity scale.
  All live-apply, no restart labels.

## Milestones

- M1: `VolumeBrick` builder + occupancy + `volumeTransfer()` in core, with
  synthetic-volume tests (gate-to-voxel roundtrip, height monotonicity,
  CPU/GPU transfer agreement). Plus `CpuVolumeView` preview at half res.
- M2: `VolumeCanvas` GL raymarcher behind the layout preset, auto-fallback
  wiring, perf HUD (brick ms, upload ms, fps).
- M3: camera presets (top-down, horizon chase view), clipping altitude slider,
  status/home integration.
- M4 (only after v1 sticks): dual-pol second channel, warning polygons as
  3D extrusions, cross-section from brick.

## Risks and honest limits

- Intel iGPU fill rate at full panel size: mitigate with half-res render
  target + upscale, standard for volume views.
- Driver variance (the reason for the CPU twin): GL 3.3 core only, no
  extensions required in v1.
- OPERA single-tilt and partial volumes look thin/patchy: label them, do not
  interpolate fake structure.
- Test coverage is core-side (brick math, transfer fn). Shaders are verified
  by compile-at-startup logging plus a manual screenshot checklist, not by
  headless pixel tests we cannot trust.

## Build status (M1 in progress)

- Core `VolumeBrick` done and tested (`src/core/volume_brick.*`,
  `testVolumeBrick`): gate splat with max-wins, 4/3-earth heights, transfer
  stops, ray-box intersect. Qt-free, shared by both backends.
- CPU preview in review: per-panel 3D toggle on `RadarMapCanvas`
  (context menu, drag orbit, wheel dolly, Esc exits), half-res march on the
  thread pool with request-id abort, palette-derived transfer stops.
- Still open: RHI spike (`QRhiWidget`, one shader set), resolution step-down
  with 2+ live 3D panels, progressive refine, dual-pol second channel.
- Brick resolution default stays 256 x 256 x 96 until profiling says otherwise.
