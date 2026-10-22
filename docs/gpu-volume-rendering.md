# GPU volume rendering of live Level II: design

Status: wired into the panel on 2026-10-21 (card t_f71a0be0). The 3D toggle
now draws through the resident raymarch and falls back to the CPU march
with the reason on the chip when the device cannot run. Since 2026-10-22
(card t_9691d611) the raymarch is one GLSL source baked to a cross-API `.qsb`,
so the GPU path runs on Direct3D 11, OpenGL and Vulkan (Vulkan measured in the
widget; the offscreen/render-thread device is D3D11/OpenGL on this kit). The CPU
march stays built and selectable (`setVolume3dBackend()`,
`CAPPEDWEATHER_VOLUME_GPU=0`). 2D stays the primary console.


## Panel integration: option 1, decided before the code (2026-10-21, card t_f71a0be0)

The spike left two ways to put the GPU march behind the panel's existing 3D
toggle. The decision is **option 1: render to an image on a worker thread and
keep `volumeImage_` as the panel's only 3D surface**, with one change to the
spike's shape: the device lives on a dedicated thread for the process and the
brick stays uploaded on it, because device creation measures 91-132 ms and a
per-frame device would eat the entire 4-8 ms win.

Why not option 2 (a `QRhiWidget` child plus a transparent HUD overlay):

- **The HUD is the panel, not a sticker.** `paintVolume3d` paints the volume
  image, the orientation grid / range rings / site marker / height axis, the
  backend chip, the Alt+drag box rectangle and the help line. Option 2 moves
  that into an overlay widget that would have to composite over a native
  child window on every platform; the grid is projected through the *same*
  camera as the image (`volumeCameraFor`), so camera state would have to be
  shared with a second widget anyway.
- **Mouse handling is 170 lines of panel code** (orbit drag with middle-drag
  focus, wheel dolly, right-click exit, Alt+drag ROI, and the gates that keep
  those from firing while measure / cross-section / pinned-card modes are
  active). A child widget receives the events, not the panel, so every one of
  those paths would need an event filter or a move. Option 1 keeps the event
  stream exactly where it is today.
- **`QWidget::grab()` is the screenshot and test path** (`volume3d_probe`,
  `readme_screenshots`, the UI suite). A native D3D11 swapchain child is not
  guaranteed to appear in a grab; an image in the panel always does.
- **Panel repaint semantics.** The 2D map underneath is a QPainter pass over
  tiles that the panel owns; a child widget that owns its own swapchain
  changes what "repaint the panel" means for every other overlay in that
  widget.

What option 1 costs, all measured: one readback per frame (1.3 MB at 720x450,
5.2 MB at 1440x900), one device for the process (91-132 ms once), and a copy
of the frame through a `QImage`. It buys the entire HUD, the ROI box, the
camera maths, the mouse handling, the screenshot path and the failure text for
free, because `volumeImage_` does not change shape.

Consequences that stay true:

- **The widget stays the proving path.** `VolumeCanvas : QRhiWidget` is not
  dead code: `volume3d_probe --gpu` still renders through it to prove the
  widget's presentation path matches the offscreen path (checksums in the
  spike section below), and `tests/ui_smoke_test.cpp` drives it. If a future
  card moves the panel to a widget, that comparison is the acceptance test.
- **The GPU frame is opaque.** The shader composites over the 3D view
  background and writes alpha 1.0, so the panel does not hit the
  premultiplied-alpha defect that makes its CPU march paint dim (spike
  section, "CPU vs GPU"). The two backends therefore differ in brightness
  until that CPU bug is fixed; the chip says which one drew the frame.
- **Resolution stays on the CPU policy** (half the panel while idle, a third
  while dragging). The GPU could march the full panel for 8-12 ms, but a
  sharper preview is a separate measured change; this card keeps GPU and CPU
  comparable ray for ray.



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

### CPU vs GPU: the gap was a CPU compositing bug, not a shading difference

The GPU frame is brighter at the same camera: lit 0.0810 vs 0.0599 at 1440x900.
That is not sampling noise. `RadarMapCanvas::rebuildVolumeRender` accumulates
proper premultiplied RGB (`r += (1-a) * colour * shade * gate`) but stored it in
`QImage::Format_ARGB32`, which QPainter interprets as **straight** alpha, so
alpha is applied a second time when the image is painted. The panel preview is
therefore dimmer than the transfer function says, and more so the lower the
per-pixel alpha — which is why the CPU number kept falling as the frame grew
(the fixed-size HUD chip stops padding the count).

Measured in the hermetic test on an identical brick and camera:

- `lit_gpu = 0.0891`
- `lit_cpu` as the panel painted it (ARGB32 + QPainter) = `0.0839`
- `lit_cpu` with the same bytes composited as premultiplied = `0.0934`

Reinterpreting the bytes as premultiplied moved the CPU image toward the GPU
image, which pinned the diagnosis (the test asserted that direction). The CPU
path was deliberately **not** changed in that card ("CPU path unchanged").

#### Fixed — 2026-10-22 (card t_cc6aa591)

The march now stores its image in `RadarMapCanvas::volumePreviewFormat` =
`QImage::Format_ARGB32_Premultiplied`, so QPainter applies the accumulated
coverage exactly once, and the packing step lives in one shared place
(`RadarMapCanvas::volumePreviewPixel`) that the march and the hermetic test both
call. No marched pixel changed value; what changed is their declared meaning.

Why ARGB32_Premultiplied and not RGBA8888_Premultiplied: both are correct, but
ARGB32_Premultiplied is the raster engine's native layout here (`0xAARRGGBB`,
what `qRgba`/`scanLine` already write), so painting needs no per-frame swizzle.
Note the original card text called `Format_ARGB32` premultiplied; it is not —
straight alpha *is* the bug.

Same volume and camera (KTLX 2013-05-20 20:12Z REF, `KTLX20130520_201229_V06`,
1440x900, brick 256x256x128, camera az 180 / el 40 / 420 km, opacity 80):

| | before (ARGB32) | after (ARGB32_Premultiplied) | GPU reference |
|---|---|---|---|
| CPU panel lit_fraction (grab, includes grid/rings/HUD) | 0.13734 | 0.15426 | 0.13691 offscreen (no chrome), 0.14479 panel * |
| CPU panel checksum | 445668258 | 477392467 * | 399665074 |
| hermetic CPU march lit_fraction (no chrome) | 0.08389 | 0.09342 | 0.08914 |

\* the GPU panel number is card t_51d7af1a's, on the branch that has the panel
GPU wiring, same volume/camera/size/product. Chrome (ground grid, rings, chip)
adds `0.14479 - 0.13691 = 0.00788` to a GPU panel grab; applying that to the
fixed CPU panel leaves `0.1464` against the GPU's `0.14479` — 0.0016 apart,
inside the `+0.0043` coverage-model difference already documented below. So the
compositing path now agrees with the GPU; what remains is the sampling model,
not alpha. The CPU panel checksum moves by ~0.003% between runs because the HUD
chip prints its own march time; `lit_fraction` is the fixed number.

The card's "0.081-after-tone-mapping" target no longer describes this tree: the
shader has no tone map (`rgb = accum + (1-alpha) * background`,
`src/app/volume_canvas.cpp`), and 0.0810 was measured on the pre-interpolation
brick. After tilt interpolation (9e6f46e) the GPU reference on this volume is
0.13691 raw / 0.14479 panel, which is the like-for-like target used above.

Pixel path pinned, red then green (hermetic, no volume data): one sample of
colour 0.5 at coverage 0.5 must store `0x7f00007f` and paint red ~131 over the
panel's `#080D10`. With the format set back to `Format_ARGB32` the new check
fails — `cappedweather_ui_tests -platform offscreen` exits 1 with
`the 3D preview applied alpha twice: painted r=67 (expected ~131 premultiplied,
~67 straight-alpha)`; with `Format_ARGB32_Premultiplied` it passes.

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
   Consequence: the spike was **D3D11 only**. *(Superseded 2026-10-22, card
   t_9691d611: qtshadertools is installed in the kit, `find_package(Qt6
   COMPONENTS ShaderTools)` is part of the build, and the shipping shader is
   now the GLSL source in `src/app/shaders` baked to `:/shaders/volume.*.qsb`.
   The HLSL string stays as the fallback for a kit without the module.)*
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

- **No wiring into the panel toggle in the spike.** That was card t_f71a0be0 and
  is done now — see "Panel integration results" below for the decision (option 1)
  and the numbers.
- Empty-space skipping (32^3 occupancy) — stretch, not required; the 4-8 ms
  frame leaves no reason to complicate the shader yet.
- Backends other than D3D11, MSAA, multi-panel fill-rate policy.

## Panel integration results — 2026-10-22 (card t_f71a0be0)

The 3D toggle now draws through the resident D3D11 march, with the CPU march as
the fallback and the reason on the chip. `volumeImage_`, the HUD, the camera
maths, the Alt+drag box and every mouse path are unchanged; only the thread that
fills that image is new.

Both screenshots are the shipping panel on the real archive
(`data/moore_replay/KTLX20130520_201229_V06`, KTLX 2013-05-20 20:12Z REF):

- `docs/screenshots/volume3d-panel-gpu.png` — chip `3D preview (GPU) ... REF 31 ms`
- `docs/screenshots/volume3d-panel-cpu-fallback.png` — broken HLSL: chip
  `3D preview (CPU) ... REF 642 ms` plus one line
  `CPU fallback: HLSL shader compilation failed: 0x80004005`, volume still drawn

### Per-frame cost, same volume, same camera, same brick

`volume3d_probe <archive> <out.png> ref 8000 --gpu` at a 1440x900 panel
(idle box, RX 7800 XT; the CPU number is a 720x450 march because the policy is
half resolution, the GPU numbers cover the panel size they name):

| path | frame | notes |
|---|---|---|
| CPU march, panel as painted | 625-659 ms | the "before" of this card |
| GPU offscreen, 1440x900 | 3.78-4.56 ms | private device, 3 frames |
| GPU resident, 1440x900 | 3.76-4.22 ms | panel's device, texture already uploaded |
| GPU resident, panel frame 720x450 | **1.79 ms** | after one orbit, `panel_gpu_settled` |
| first frame on a fresh device | 108-143 ms | device + 16.8 MB upload, once |
| first panel frame after a resize | 31.1-31.4 ms | includes the HLSL pipeline compile |
| toggle -> first frame on screen | 198-236 ms | brick build + device + march |

The brick is uploaded once and stays resident: `uploads=1 frames=2` across the
two resident frames, and the UI suite proves the policy (`uploads=2 frames=4`
for two frames on one brick, a resize, and one changed brick). GPU and
offscreen frames are pixel-identical for the same camera (`checksum=331428282`
on both), and `lit_fraction=0.08097` at 1440x900 matches the spike.

### 2D paint while the 3D panel is live

`pan_benchmark` (live KJAX, `grab()` of panel 0 after each drag step, i.e. a
full synchronous 2D repaint):

| run | samples | median | p95 |
|---|---|---|---|
| 1x1, no 3D | 40 | 23.03 ms | 63.32 ms |
| 1x2 control (`--pair`), panel 1 on the 2D map | 60 | 14.69 ms | 19.68 ms |
| 1x2 control, second run | 40 | 16.20 ms | 20.83 ms |
| 1x2, GPU 3D panel orbiting | 60 | **14.78 ms** | **17.79 ms** |
| 1x2, GPU 3D panel orbiting, second run | 40 | 17.17 ms | 44.82 ms |
| 1x2, GPU 3D panel orbiting, third run | 40 | 15.83 ms | 19.45 ms |

Median is flat against the same-layout control (+0.6%); p95 is not a stable
statistic at 40-60 samples on a live feed (one slow overlay rebuild moves it:
17.8, 19.5, 44.8 ms across three identical runs). What the numbers do establish
is that a live GPU 3D panel does not stall the 2D paint path: the GPU thread
marches, the GUI thread paints, and the 2D median stays where the control is.
The 1x1 row is not a comparison — panel 0 is wider there, so it paints more.

### Fallback, proved with a broken shader

`CAPPEDWEATHER_VOLUME_BREAK_SHADER=1 volume3d_probe ... --gpu`:

```
gpu_offscreen failed error="HLSL shader compilation failed: 0x80004005"
gpu_widget ready=0 backend=D3D11 frames=0 status="HLSL shader compilation failed: 0x80004005"
gpu_resident failed error="HLSL shader compilation failed: 0x80004005"
3D: GPU raymarch failed, using the CPU march - HLSL shader compilation failed: 0x80004005
panel_gpu backend=CPU render_ms=642.0 gpu_error="HLSL shader compilation failed: 0x80004005"
panel_gpu_settled backend=CPU render_ms=709.00 frames=0 applied_ms=8365.0
cpu backend=CPU lit_fraction=0.05990 checksum=327495561 render_ms=616.0
```

`frames=0` on the second line is the sticky part: after one failure the panel
does not keep retrying the device per orbit, it marches on the CPU and says why.
Toggling the 3D view off and on again is treated as a deliberate retry.

### Honest limits of this integration

- **The chip's `ms` is a march time, not a presented frame.** It is the readback
  interval on the GPU thread, the same field the CPU path always reported.
- **The first GPU frame of a panel is slower than the rest** (31 ms at 720x450):
  the shader pipeline is compiled on the first draw after a new render target.
  The frame after it is 1.8 ms. Nothing is smoothed over; the chip shows what
  the frame cost.
- **Resolution stays on the CPU policy** (half resolution idle, a third while
  dragging), so the GPU is not being asked to do more than the CPU was. Raising
  it is a separate measured change.
- **One process-wide device, one frame at a time.** Two 3D panels serialize on
  the GPU thread; each still gets 2-8 ms frames because the texture is resident.


## The chip names the backend, and the resident texture follows the brick — 2026-10-22 (card t_51d7af1a)

Two things were missing once the wiring landed on `main`: a reader could not tell
which raymarcher drew the frame, and the resident 3D texture was created once and
never resized when a brick with a different grid arrived on the same device.

### Chip

`3D  <radar> <product>  <ms> ms  (<backend>)` followed by the existing box and
interpolation disclosures, so the operator reads the backend and the honesty
caveats on the same line:

- GPU frame: `3D  KTLX REF  50 ms  (Direct3D 11)  - 17 tilts interpolated (78% reconstructed)`
  (`docs/screenshots/volume3d-chip-gpu.png`)
- CPU frame: `3D  KTLX REF  1154 ms  (CPU)  - 17 tilts interpolated (78% reconstructed)`
  (`docs/screenshots/volume3d-chip-cpu-fallback.png`)

`(Direct3D 11)` is the string the RHI reported (`rhi_->backendName()`), not a
constant, so a GL shader set (card t_9691d611) will name OpenGL without a code
change. The frame's backend and its cost are written together by `publish()`, and
`volumeFrameGpu_` no longer flips optimistically at request time: a chip that says
`(Direct3D 11)` is always describing the pixels on screen. A panel with no frame
yet reads `3D  KTLX REF  rendering (GPU)`, never a backend paired with a cost it
did not measure. Returning to a backend (`setVolume3dBackend`) drops the old
frame, its label and its ms, so the transition cannot show the previous
backend's number under the new backend's name.

The text rules are pinned hermetically in `ui_smoke_test` through
`RadarMapCanvas::volume3dChipLabel(Volume3dChipState)`: a GPU frame names its
API, a CPU frame never names one (even with a stale backend string), and no cost
is claimed before a frame has measured one.

### The crash the test found

`cappedweather_ui_tests -platform offscreen` died with exit 139 the moment the
resident device was asked for a brick whose grid was not the one already
resident: the 3D texture was created on first upload and then reused for every
later brick, so a 40x40x20 upload went into a texture still sized 256x256x128.
D3D11 walked the old row pitch off the end of the byte array — an access
violation inside the QRhi batch (`sev=c0000005` inside
`endOffscreenFrame()`), not a failure the caller could catch. It reproduced only
when a differently-shaped brick followed another on the same device, which is why
the probe (one grid per run) never saw it.

The fix keeps the texture's grid and depth next to it and replaces the texture
when the brick changes shape, recreating the SRB and pipeline with it (they bind
the texture by pointer). `ui_smoke_test` now marches a 24x24x12 brick on the
device that just uploaded a 40x40x20 one: `uploads=3 frames=5` and a non-null
image. Before the fix that frame was the crash.

### Numbers, same binary, real archive, 1440x900

`volume3d_probe data/moore_replay/KTLX20130520_201229_V06 <out> ref 8000 --gpu
--size=1440x900` (KTLX 2013-05-20 20:12Z, RX 7800 XT, one machine):

| path | frame | lit_fraction | note |
|---|---|---|---|
| shipping panel, CPU backend | **1254 ms** | 0.13713 | what a user had before the wiring |
| shipping panel, GPU backend (first frame) | 50.4 ms | 0.14938 | includes the pipeline compile |
| shipping panel, GPU backend (after one orbit) | **1.79 ms** | 0.14479 | `panel_gpu_settled` |
| GPU offscreen frame | 4.44 ms | 0.13691 | private device, 3 frames |
| GPU resident frame | 6.02 ms | 0.13691 | texture resident, `uploads=1 frames=2` |

The panel rows carry the HUD (grid, rings, chip), so their `lit_fraction` is not
comparable to the raw frames; the structural comparison is GPU 0.13691 against
the CPU panel 0.13713 (0.16% apart) at the same camera, plus the hermetic
GPU/CPU frame check in `ui_smoke_test` (0.0891 vs 0.0839 lit).

### Fallback, proved with a broken shader at 1440x900

`CAPPEDWEATHER_VOLUME_BREAK_SHADER=1 volume3d_probe ... --gpu --size=1440x900`:

```
gpu_offscreen failed error="HLSL shader compilation failed: 0x80004005"
3D: GPU raymarch failed, using the CPU march - HLSL shader compilation failed: 0x80004005
panel_gpu backend=CPU render_ms=1154.0 toggle_first_frame_ms=1517.0 lit_fraction=0.13914 gpu_error="HLSL shader compilation failed: 0x80004005"
panel_gpu_settled backend=CPU render_ms=1320.00 frames=0 applied_ms=8409.0 lit_fraction=0.13583
cpu backend=CPU lit_fraction=0.13712 checksum=444961060 render_ms=1179.0
```

The panel drew a volume (`lit_fraction=0.13914`, not a black panel), the chip
said `(CPU)`, and the one line under it named the reason.

### Still open

- The `ms` remains a march time, not a presented frame (unchanged by this card).
- Metal is baked but not measured (no macOS here); see the cross-API section
  below.

## Cross-API shader set — 2026-10-22 (card t_9691d611)

The raymarch used to be an HLSL string compiled at runtime, so `create()` had to
refuse every backend except D3D11. That is gone: the shader is GLSL 450 in
`src/app/shaders/volume.{vert,frag}`, Qt Shader Tools bakes it at build time into
`:/shaders/volume.{vert,frag}.qsb`, and each `.qsb` carries **SPIR-V (Vulkan),
GLSL 150 (OpenGL), HLSL 5.0 (D3D11) and MSL 1.2 (Metal)**. `volume_canvas.cpp`
loads the qsb whenever the build has it, keeps the HLSL string as the
D3D11-only fallback for a kit without Shader Tools, and accepts D3D11, OpenGL,
Vulkan and Metal. D3D12 is refused by name: it needs HLSL 6/DXIL stages the bake
does not produce.

Two build details that cost time and are now pinned in `CMakeLists.txt`:

- The bake targets are `GLSL 150 HLSL 50 MSL 12`, not the Qt default
  `GLSL 100 es,120,150`: the march loop bound (`maxSteps`) is a uniform, and
  GLSL 100 es/120 require constant loop bounds. SPIR-V is always baked and
  QRhi's OpenGL backend needs GL 3.3 / GLSL 150 anyway.
- The qrc is attached to each executable (`CappedWeather`,
  `cappedweather_ui_tests`, `volume3d_probe`, `pan_benchmark`), not to
  `cappedweather_ui`: a resource inside a static library is dropped by the
  linker unless a consumer calls `Q_INIT_RESOURCE()`.

`CAPPEDWEATHER_VOLUME_RHI=opengl|vulkan|d3d11|d3d12|metal` forces the widget and
the offscreen/render-thread device, so one binary can be measured on every API
the machine has. `volume3d_probe --backend=<name>` sets it and prints
`backend_requested`, `backend=`, `shader=` (`qsb` or `HLSL`) and `api=` next to
the existing numbers.

`CAPPEDWEATHER_REQUIRE_SHADER_TOOLS` (default OFF, env override) makes a release
configure fail instead of silently shipping the HLSL/D3D11-only path. Proved
both ways on this kit:

```
-DCMAKE_DISABLE_FIND_PACKAGE_Qt6ShaderTools=ON -DCAPPEDWEATHER_REQUIRE_SHADER_TOOLS=ON
  CMake Error at CMakeLists.txt (message): CAPPEDWEATHER_REQUIRE_SHADER_TOOLS=ON
  but Qt6::ShaderTools was not found   [exit 1]
-DCMAKE_DISABLE_FIND_PACKAGE_Qt6ShaderTools=ON -DCAPPEDWEATHER_REQUIRE_SHADER_TOOLS=OFF
  -- Qt Shader Tools (qsb) not found: the 3D volume keeps the HLSL/D3D11-only shader
  -- Configuring done   [exit 0]
```

### Backends that actually render on this machine

Real archive `data/moore_replay/KTLX20130520_201229_V06` (KTLX 2013-05-20
20:12Z, REF, 256x256x128 brick), RX 7800 XT, Qt 6.8.3 / MSVC 2022, 2026-10-22.
"Widget" is the `VolumeCanvas` QRhiWidget; "resident" is the process-wide device
the shipping panel draws through.

| backend | widget | offscreen | resident | panel | shader |
|---|---|---|---|---|---|
| Direct3D 11 | frame, 0.13691 lit | frame, 4.82 ms | frame, 6.43 ms | GPU, 2.69 ms settled | qsb (HLSL 5.0) |
| OpenGL (4.6 compat) | frame, 0.13691 lit | frame, 3.03 ms | frame, 2.99 ms | GPU, 2.26 ms settled | qsb (GLSL 150) |
| Vulkan | **frame, 0.13691 lit** | refused (no `vulkan.h`) | refused (no `vulkan.h`) | CPU, named reason | qsb (SPIR-V) |
| D3D12 | refused (`no shader set for the D3D12 backend`) | refused | refused | CPU, named reason | none |
| Metal | not measured (no macOS) | not measured | not measured | not measured | qsb (MSL 12) |

The Vulkan widget frame is the SPIR-V variant: `gpu_widget ready=1 backend=Vulkan
shader=qsb frames=4 lit_fraction=0.13691`. The panel refuses Vulkan *in the
resident device only*, because `QRhiVulkanInitParams` needs `<vulkan/vulkan.h>`
at build time and this kit ships none; QRhiWidget presents Vulkan because Qt
owns the VkInstance internally.

### Same volume, same camera, 1440x900 — the two shipping backends

```
volume3d_probe <V06> <out> ref 6000 --gpu --size=1440x900 [--backend=opengl]

                    D3D11                       OpenGL
gpu_offscreen       4.82 ms  lit 0.13691         3.03 ms  lit 0.13691
                    checksum 399665052           checksum 399665022
gpu_widget          frames=4 lit 0.13691         frames=4 lit 0.13691
gpu_resident        6.43 ms  lit 0.13691         2.99 ms  lit 0.13691
                    uploads=1 frames=2           uploads=1 frames=2
panel_gpu           89.7 ms  lit 0.14939         36.2 ms  lit 0.14930
panel_gpu_settled   2.69 ms  lit 0.14480         2.26 ms  lit 0.14441
cpu reference       1807 ms  lit 0.13714         1378 ms  lit 0.13713
```

The two backends agree: identical `lit_fraction` on the raw frames, checksums
30 apart out of 3.99e8 (floating-point rounding in a different rasterizer), and
the panel rows differ by 0.06% of lit pixels. The D3D11 image is unchanged from
the HLSL-string shader: `lit_fraction` 0.13691 and the same panel numbers as the
t_51d7af1a run. The offscreen `ms` (4.44 -> 4.82) is box load, not the shader —
the CPU reference in these runs was 1807 ms against 1254 ms in that one.
Screenshots: `docs/screenshots/volume3d-panel-gl.png` (panel chip names
`(OpenGL)`) and `docs/screenshots/volume3d-widget-vulkan.png` (widget frame on
Vulkan).

### CPU fallback when a backend cannot come up

`--backend=vulkan` (offscreen/resident refuse) and `--backend=d3d12` (no HLSL 6
in the bake) both end with a CPU frame and the reason in `gpu_error`, not a black
panel:

```
panel_gpu backend=CPU api= render_ms=1259.0 toggle_first_frame_ms=1541.0 lit_fraction=0.14002 gpu_error="GPU 3D offscreen has no Vulkan device in this build (no vulkan/vulkan.h); the widget path still accepts CAPPEDWEATHER_VOLUME_RHI=vulkan"
panel_gpu backend=CPU api= render_ms=1357.0 toggle_first_frame_ms=1670.0 lit_fraction=0.13970 gpu_error="GPU 3D offscreen has no device for CAPPEDWEATHER_VOLUME_RHI=d3d12"
```

`CAPPEDWEATHER_VOLUME_BREAK_SHADER=1` now poisons the *qsb* pair instead of the
HLSL source, so the common path is the one proved to fall back:

```
gpu_offscreen failed error="No HLSL (shader model 5.0) code found in baked shader QShader(...)"
gpu_widget ready=0 backend=D3D11 shader=broken frames=0 ...
panel_gpu backend=CPU render_ms=1184.0 toggle_first_frame_ms=1511.0 lit_fraction=0.13996 gpu_error="No HLSL (shader model 5.0) code found in baked shader QShader(...)"
```

A backend that never brings up an RHI device at all used to leave the widget on
"initialising" forever; it now reports through `QRhiWidget::renderFailed()` into
the same one-line `gpuFailed()` path.

### Known Qt caveat

Creating the GL fallback surface on the render thread prints
`Attempting to create QWindow-based QOffscreenSurface outside the gui thread.
Expect failures.` on Qt 6.8.3/Windows. The frames still complete — resident
`uploads=1 frames=2`, 2.99 ms, `lit_fraction=0.13691`, checksum identical to the
offscreen GL frame — but the warning is recorded here so it is not a mystery in a
support report.

### Repro

```
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH"
export QT_PLUGIN_PATH="C:/Qt/6.8.3/msvc2022_64/plugins"
./build/Release/volume3d_probe.exe data/moore_replay/KTLX20130520_201229_V06 \
  "$SCRATCH/gl" ref 6000 --gpu --size=1440x900 --backend=opengl
```

