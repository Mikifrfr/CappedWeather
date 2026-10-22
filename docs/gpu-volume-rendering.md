# GPU volume rendering of live Level II: design

Status: M2 spike measured 2026-10-21 (card t_710dd647). The raymarch renders on
D3D11; it is not wired into the panel toggle yet (see "Spike results" below for
what is proved and what is not). 2D stays the primary console.


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

## Spike results — 2026-10-21 (card t_710dd647)

Scope: one widget, one shader set, one product, D3D11. The CPU march is
untouched and remains the fallback.

### What exists now

- `src/app/volume_camera.hpp` — `VolumeCamera` + `volumeCameraFor()`, moved out
  of `radar_map_canvas.cpp` so the CPU march and the RHI march shoot the same
  rays from one implementation (same math, behaviour unchanged).
- `src/app/volume_canvas.{cpp,hpp}` — `VolumeCanvas : QRhiWidget` (a real
  window, `setVolume` / `setTransferStops` / `setOrbit`), plus
  `renderVolumeOffscreen()` which renders the same brick and camera into a
  texture, reads it back and waits for the GPU: the number the probe and the
  test use.
- `RadarMapCanvas::volumeTransferStops()` — one ramp builder for the CPU march
  and the GPU palette LUT, sampled through the shipped palette path. The CPU
  block that used to inline those 48 stops now calls it; the stops are
  identical.
- `tools/volume3d_probe ... --gpu` — renders the same archive volume through
  the widget, offscreen, and the shipping CPU panel path, then prints the three
  lit_fraction numbers.
- `tests/ui_smoke_test.cpp` — hermetic GPU/CPU comparison on an in-memory brick
  (prints a note and skips the comparison if D3D11 is unavailable; CI without a
  device does not fail).

### Numbers (KTLX 2013-05-20 20:12Z REF, `KTLX20130520_201229_V06`)

Brick 256 x 256 x 128, 1171.9 m voxels, 251,336 measured voxels. Camera:
azimuth 180, elevation 40, 420 km, target z 7000 m, echo opacity 80.

| size | CPU march (panel path) | GPU offscreen frame | GPU first frame (incl. upload) | lit GPU | lit CPU (as painted) |
|---|---|---|---|---|---|
| 720x450 | 241 ms | 2.3–4.9 ms | 112–117 ms | 0.0810 | 0.0757 |
| 1440x900 | 802–950 ms | 3.8–8.1 ms | 91–132 ms | 0.0810 | 0.0599 |
| 2880x1800 | 2183 ms | 17.9 ms | 129 ms | 0.0810 | 0.0436 |

The GPU lit_fraction is resolution-independent to five decimals (same rays, same
brick). The target (<16 ms at 1440x900) is met with margin: 3.8–8.1 ms across runs
on a 7800 XT. "First frame" is device creation plus the 16.8 MB RG8 volume
upload; a one-off, not a per-frame cost. GPU memory for the brick is 16.8 MB.

Widget path: the real `VolumeCanvas` renders the same image as the offscreen
render at 1440x900 (lit_fraction 0.08097 both, identical checksum), so the
widget's presentation path does not alter the result. Screenshots:
`docs/screenshots/volume3d-gpu-widget.png` (widget),
`volume3d-gpu-offscreen.png` (offscreen), `volume3d-gpu-cpu.png` (shipping CPU
panel path, same volume and camera, 950 ms chip).

### CPU vs GPU: the gap is a CPU compositing bug, not a shading difference

The GPU frame is brighter at the same camera: lit 0.0810 vs 0.0599 at 1440x900.
That is not sampling noise. `RadarMapCanvas::rebuildVolumeRender` accumulates
proper premultiplied RGB (`r += (1-a) * colour * shade * gate`) but stores it in
`QImage::Format_ARGB32`, which QPainter interprets as **straight** alpha, so
alpha is applied a second time when the image is painted. The panel preview is
therefore dimmer than the transfer function says, and more so the lower the
per-pixel alpha — which is why the CPU number keeps falling as the frame grows
(the fixed-size HUD chip stops padding the count).

Measured in the hermetic test on an identical brick and camera:

- `lit_gpu = 0.0891`
- `lit_cpu` as the panel paints it (ARGB32 + QPainter) = `0.0839`
- `lit_cpu` with the same bytes composited as premultiplied = `0.0934`

Reinterpreting the bytes as premultiplied moves the CPU image toward the GPU
image, which pins the diagnosis (the test asserts that direction). The CPU path
was deliberately **not** changed in this card ("CPU path unchanged"); the fix is
a follow-up card.

Second, smaller difference: the shader's coverage is the hardware-filtered
weight of present corners (`sum w`), while `sampleBrickTrilinear` reports
`count / 8`. Both are 1 inside data and 0 outside; they differ only in the
sub-voxel falloff at hole edges, so the GPU edge is smoother rather than
denser. Matching `count / 8` exactly would cost eight nearest taps per step for
the sample plus forty-eight for the shading normal — 2-4x the frame time for a
difference that is invisible at 1440x900. Reported, not papered over.

### Findings that cost time (all reproduced on this box)

1. **`QRhiWidget` does not open the render pass.** Qt 6.8.3 hands `render()` a
   command buffer with no pass active; calling `cb->draw()` straight away
   segfaults inside QRhi with no warning. `render()` must
   `cb->beginPass(renderTarget(), colour, {1.0f, 0})` and `endPass()` itself.
   Minimal reproduction: a QRhiWidget drawing one full-screen triangle crashes
   without `beginPass` and renders three frames with it.
2. **This Qt install has no Qt Shader Tools** (`bin/qsb` absent, no
   `Qt6::ShaderTools`, no `QShaderBaker`), so GLSL/SPIR-V could not be compiled
   into a cross-API `.qsb`. The shader is HLSL (Shader Model 5) built into a
   `QShader` at runtime; QRhi's D3D11 backend compiles it through D3DCompile.
   Consequence: the spike is **D3D11 only**. Vulkan/Metal/GL need the Shader
   Tools module in the build (or checked-in `.qsb` files); a packaging decision
   for the cross-API plan, not a code problem.
3. **`QRhiTexture::OneDimensional` fails on D3D11** in this Qt build, so the
   palette LUT is a 256x1 2-D texture sampled at `u = (255*t + 0.5)/256`.
4. **3-D texture uploads need one entry per depth slice.** A single
   whole-volume `QRhiTextureUploadEntry(0, 0, data)` created without error and
   sampled as an all-zero texture (a black frame with no error). 128 slice
   entries work.
5. **The widget's render target carries a depth-stencil buffer**; the pipeline
   is created against `renderTarget()->renderPassDescriptor()` and recreated
   when Qt swaps that descriptor. Qt calls `initialize()` again in that case.
6. **Colour agreement** was checked by eye on all three screenshots (same
   diagonal storm, same core) and numerically by lit_fraction (GPU identical
   across resolutions and between widget and offscreen).

### Failure behaviour (proved, not asserted)

With the HLSL deliberately broken, the probe prints one line and the CPU path
still produces a frame:

```
gpu_offscreen failed error="HLSL shader compilation failed: 0x80004005"
gpu_widget ready=0 backend=D3D11 frames=0 lit_fraction=0.00000 status="HLSL shader compilation failed: 0x80004005"
cpu lit_fraction=0.07569 checksum=89923316 render_ms=316.0 screenshot=/tmp/failcase.png-cpu.png
volume3d_probe=gpu_unavailable error="HLSL shader compilation failed: 0x80004005"
```

The widget never presents a black frame with no explanation: `statusLine()`
carries the reason and `gpuFailed()` fires once. Texture/LUT creation failures
and backend mismatches take the same path (`GPU 3D needs the Direct3D 11
backend; this spike ships no shader set for <backend>`).

### Not done (deliberately)

- **No wiring into the panel toggle.** The 3D view is painted by
  `RadarMapCanvas` with QPainter (image + orientation grid + chip + drag
  rectangle). A full-size `QRhiWidget` child would cover that HUD, so making the
  GPU path the panel's backend needs one of: (a) GPU render to an image on the
  existing worker thread (adds a readback, keeps the HUD untouched), or (b) the
  widget plus a transparent HUD overlay. Both are integration decisions with
  their own measurements — a follow-up card, not part of the spike.
- Empty-space skipping (32^3 occupancy) — stretch, not required; the 4-8 ms
  frame leaves no reason to complicate the shader yet.
- Backends other than D3D11, MSAA, multi-panel fill-rate policy.
