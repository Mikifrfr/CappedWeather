# CappedWeather TODO

Short active list. Done work stays in git history and release notes, not in this file.

## Open

### Analysis depth
- [ ] Rainfall accumulation (one-hour and storm-total style) from Z-R over spill-to-disk history. Needs AP/clutter guard plus validation before any trust. No precision claims until then.
- [ ] OPERA dual-pol unlock when 2027 moments land (CC, ZDR, PHI, KDP). Work is decoder mapping in `readOdimPvol` plus a fixture and availability tests. Per-panel auto-disable already handles their absence, so do not fake dual-pol from single-pol before then.

### Overlays and context
- [ ] MTG Lightning Imager for European sessions. GLM covers GOES-18/19 only: MTG LI L2 lives behind EUMETSAT Data Store registration plus OAuth (no anonymous feed like NOAA S3), so this needs a user-supplied credential flow first. No code until then.

### Playback and export
- [ ] GIS export follow-on: GeoTIFF for one frame (needs a native TIFF dependency on every platform plus CI wiring). KML GroundOverlay is wired; uses current georeferencing only, no reprojection magic.

### Platform and UX
- [ ] macOS build. Windows installer (NSIS + ZIP) and Linux AppDir TGZ are shipped from tags. macOS is the remaining stretch.
- [ ] Flatpak. Artifacts already ship, discovery is the gap. (Windows update discovery is done: Help action checks the public releases metadata with explicit current/newer/no-info/error states, discovery only.)
- [ ] Light / high-contrast theme variant. Optional. Dark and Classic are complete including focus rules and dialog coverage.
- [x] Zone disk cache live check (`tools/overlay_probe.cpp`, 2026-10-11, 306+ active alerts: cold launch 284 requests, warm launch 83 (same 12 unparseable zones refetch by design, rest is new-alert churn plus base feeds), warnings complete both runs (412/413). ~3.4x fewer requests.
- [x] Cold-start time to first echo (`tools/volume_timing_probe.cpp`, 2026-10-11: KGSP first complete live volume 5.8 s after launch with opt-out defaults, overlays still fetching in background. Radar never waits on context layers.

### Research (explicitly not next)
- [ ] GPU volume rendering of live Level II. Design in `docs/gpu-volume-rendering.md` (shared VolumeBrick, GL raymarcher plus CPU twin). Not started.
- [ ] Storm-scale warn-on-forecast nowcasting. Motion extrapolation (+10/+20/+30 min) is done; ML needs infra plus labeled archives.
- [ ] Level III / national mosaic as optional context. Level II stays first.
- [ ] Broadcast / layout export packs. Out of scope unless product direction changes.

## Proposed (researched, not committed)

Ready to build (data live now, testable hermetically plus replay):
- [ ] Dual-pol QPE for NEXRAD. R(Z,ZDR)/R(KDP) accumulations next to Z-R over spill history, same AP/clutter guard requirement as the rainfall item. No trust until gauge/MRMS comparison.

Research-gated (confirm open shape first, METAR/ECCC style):
- [ ] Nearest-radar mosaic. PROTOTYPED 2026-10-10 (`tools/mosaic_probe.cpp`, scored in `docs/mosaic-seams.md`): same-tilt max/nearest compositing fails on Moore 2013 KTLX+KINX (beams differ ~5 km on the headline storm, both-echo RMSE 10.6 dB, only 73/815 cells beam-match to 4 km). CAPPI matching scored 2026-10-11 on the same pair (`--cappi`: RMSE 9.67, beam-matched cells 5-6 dB, 6.3x more comparable cells; SAILS-duplicate selection caution documented). Second case scored 2026-10-11 (Mayfield KPAH+KVWX from the public unidata archive: baseline RMSE 8.85, CAPPI 7.46-7.51, paired same-cell verdict only +4% at 1500 m and negative at 2500 m). Mosaic earns a layer only with sub-volume time sync on top; the residual is timing, not altitude selection.

## Non-goals (for now)
- Full GR-style product scripting language
- Matching proprietary basemap imagery
- Becoming AWIPS or a TV automation suite
- Shipping synthetic or demo radar frames

## Recently done
- Shared GOES georeference across panels (one immutable lon/lat grid per exact grid identity in `GoesGeoCache`, built once off the GUI thread with per-identity waiter coalescing; canvases attach by identity so rapid swaps, disable, close and nullopt fail safe; 2x2 cold wall 2116 to 51 ms with exactly one 30 MB grid; honest waiting state while building; committed sharing/swap/disable/destroy lifecycle tests).
- NWS zone geometry disk cache (api.weather.gov zone bodies persist under CacheLocation, 7-day reuse without any request, 30-day prune, conditional revalidation after, one write per refresh round; warm starts skip unchanged geometry while alerts/reports stay live; live Dallas County TXC303 fixture plus round-trip/expiry/corrupt/preload hermetic tests).
- NWS zone GeometryCollection members (census of today's 246 live zone shapes: the 12 standing "no polygon geometry" failures are all coastal FLZ forecast zones grouping mainland plus island MultiPolygons, verified against live FLZ050; parser recurses into members through the existing MultiPolygon path, minimal collection fixture test).
- GOES viewport oracle (false-empty candidate refuted: 9/9 view matrix MATCH on the real 3.75M-cell C13 frame including zoom-3 antipode, antimeridian wrap, both limbs; test-only exhaustive-scan flag plus committed synthetic-patch subwindow regression, mutation-killed; bench cold path fixed to own the georeference build: decode 123 ms, true cold 334-536 ms once per frame, swap 319-493 ms, warm 7-21 ms whole-canvas).
- GOES paint
 
...[truncated 305 chars]
- Windows update discovery (Help action plus silent weekly auto-check, public stable-release metadata with explicit current/newer/no-info/error states, host-validated releases link, no downloads; hermetic version/parser/clock tests).
- ESRI shapefile placefiles (binary .shp polygon/polyline rings as Polygon/Line elements via magic sniff in both load paths, unprojected lon/lat only with an explicit projected-data error, sibling .dbf names as hover text, same safety envelope as text placefiles, hermetic parser plus source tests on Census-county fixtures).
- GRLevelX color-table import (Product/Units/Step/Decimals/Scale headers plus Color/Color4/SolidColor rules into custom step stops, sorted, fail-closed with line numbers, live RTWX_REF shape verified, hermetic parser tests).
- IEM NEXRAD national mosaic (5-minute N0Q archive PNG plus world file, exact filename timestamps, scaled decode to paint budget, native colors under echo with scan time, layer with badge and persisted toggle, hermetic parser/decode/paint tests on a committed fixture).
- Case-pack export/import (.cwcase zip of spilled volumes plus manifest plus placefiles, File menu actions with progress and cancel, shared tested manifest/zip/rebuild logic with fault-in coverage).
- Nearest-radar mosaic seam probe (`tools/mosaic_probe.cpp`, Moore 2013 KTLX+KINX scored in `docs/mosaic-seams.md`: same-tilt compositing fails on beam mismatch, mosaic stays a probe until lowest-beam-wins or CAPPI matching lands).
- GOES ABI infrared (anonymous S3 RadC thermal bands 7-16 brightness temperature via fixed-grid remap validated against the file's own extent record, unsigned-count and valid-range hardening proven by a band-8 fixture, observed context under echo with scan time and band picker, layer with badge and persisted toggle, hermetic tests on committed fixtures).
- HRRR simulated reflectivity (TDS NCSS REFC subset, analysis plus 1/2/3/6/12 h forecast leads on one picker, Lambert math validated against the live grid, model-context raster under echo with valid hour, layer with badge and persisted toggle, hermetic tests on a committed fixture).
- RAOB sounding hodograph (IEM 00Z/12Z fetch with station memory, height-colored trace plus storm-motion vector, sounding-vs-VAD SRH ground truth sharing one integrator, freezing-level readout, hermetic parser/clock/paint tests, live-verified payload shape).
- NWS DAT survey paths (90-day recency-bounded GeoJSON with EF colors, line layer with badge and persisted toggle, live-verified query shape, hermetic parser and color tests).
- HMS fire plus smoke overlay (daily detection text with satellite/FRP plus KML plumes with density/times, official colors, today-then-yesterday fallback, clean-air quiet state, layer with badge and persisted toggle, live-verified, hermetic parser and URL tests).
- WPC excessive-rainfall outlooks Day 1-3 (Esri REST GeoJSON with official renderer colors, layer with badge and persisted toggle, live-verified against today's SE US ERO, hermetic parser tests).
- NHC tropical cyclone overlay (active-storm KMZ discovery plus cone/track KMZ geometry, layer with badge and persisted toggle, live-verified during Isaias/Rachel/Simon, hermetic parser and KMZ-extraction tests).
- Precip-type map (METAR surface temps crossed with the measured melting level, dotted station rings plus card line, strict fail-closed gating, hermetic classifier tests, experimental until a winter-season scoring).
- Hail swaths from tracks plus cores (per-storm convex-hull footprints of persisted hail fixes, painted under the hail toggle, Placemark polygons in the frame KML export, six-case core tests plus KML builder tests).
- TVS/meso markers into AlertCenter (opt-in sink off by default, TVS at Severe+ and meso at Moderate+, radar-scoped dedup with suppression and priming, always labeled guidance, Settings checkbox with persisted key, hermetic ingest tests).
- Storm-relative helicity 0-1/0-3 km from VAD plus Bunkers motion (pair-sum integration with per-layer span gates, scan pop-out readout with quality flags, closed-form core tests, documented in derived-products).
- Bunkers storm motion from VAD (0-6 km gateCount-weighted mean wind with 7.5 m/s shear-perpendicular split, weak-shear flag, VAD-style quality, right-mover line in the scan pop-out, closed-form core tests).
- TBSS hail-spike detector (storm-edge-rooted weak-echo runs with fade-out ends, 20 km core link, cross-tilt proximity clustering, sheet rejection; map layer with badge and persisted toggle; ten-case core tests; replay first-light on Mayfield plus withhold census in severe-hail-tds-validation).
- Basemap preset gallery (one-click IEM US + RainViewer global mosaic templates in Settings, keyless Custom no longer falls back to Esri, RainViewer frame resolver with hermetic tests, live 200-PNG verification, documented in europe-sources).
- Warning inbox expiry countdowns (per-item time-to-expire from onset/expiry, soonest-first within severity, 30 s ticker, hermetic clock tests).
- SPC fire-weather outlooks Day 1/2 (DryT + WindRH GeoJSON, same schema as categorical, official orange colors, layer with badge and persisted toggle, live-shaped parser tests).
- Bright-band melting-level estimate per volume (CC-dip ring median with quality flags, shown in the scan pop-out; future ZDR-column gating).
- ZDR columns as updraft markers (dual-pol stacks over 1.5 km on 2+ tilts topping 3 km, layer with badge and persisted toggle, core tests plus replay-ready).
- World-file sidecars (.pgw) for snapshots and KML frames so every PNG loads in GIS.
- Environment Canada alerts (open MSC GeoMet OGC feed, Canada-wide bbox, shared warning rows/colors/probe/cards with NWS, last-known-good on failure).
- Case-pack import session rebuild as shared tested logic (copy plus decode check per volume, staged cleanup, radar guard) with fault-in coverage.
- Spill prefetch fans out to every panel session for mixed-radar layouts.
- Pinned-card keyboard scroll (Up/Down when the card overflows, same target physics as the wheel, editing widgets keep their keys). Shortcut help lists it.
- Full font-scale chrome (dock titles, badges, tabs, captions follow the interface text preference).
- Offline basemap pack for field chasing (region/zoom plan with tile cap and byte estimate, downloads into the shared tile cache through the production URL builder, verified against live tiles with a headless CLI).
- KML frame export for GIS (PNG snapshot plus GroundOverlay with the panel lon/lat box, XML-escaped, fail-closed on wrap or flipped bounds). GeoTIFF stays open (native dependency).
- WebM timeline export (frame-exact VP9 through an ffmpeg image pipe at GIF timing, cancellable with progress, fail-closed dialog when ffmpeg is missing). GIF stays frame-exact without dependencies, MP4 stays window capture.
- Scan diagnostics full panel: per-tilt table pop-out (rays, gaps, VAD wind, height, RMSE, fit quality) live with the timeline, next to the live dock readout.
- METAR surface observations overlay (aviationweather.gov station catalog plus batched obs, newest per station, flight-category colors). Layer with badge, pinned cards, persisted toggle, region follows the selected radar. Probe shows temp/dew of the nearest station.
- VAD wind profile per tilt (least-squares sine fit on dealiased velocity with Good/Marginal/Poor/Invalid quality) inside the scan diagnostics pop-out (per-tilt rays, gaps, wind, height, RMSE, live with the timeline).
- Spill-to-disk background prefetch (nearest spilled neighbours fault in off the UI thread, installed by volumeId so tail-drop shifts stay safe). Pick-list and install covered by tests.
- SPC Day 1 + 2 categorical outlooks in official colors plus active mesoscale discussions with watch probability (RSS discovery, discussion-page parse). Layers with badges, pinned cards, persisted toggles. Watches ride NWS alerts, no new code.
- Composite reflectivity plus CAPPI at selectable MSL altitude (2/3/4/5/7 km, global setting with background re-derive and spill-safe altitude tags). REF packing shared, so palettes and probes agree.
- Hail cores from VIL density (Amburn and Wolf tiers, guidance size ranges not diameters, cluster 10 km by warning-CSI sweep) plus polarimetric debris signatures (CC drop at live couplets with contrast guard and 2-volume persistence). Scored on three archive cases in `docs/severe-hail-tds-validation.md`. Guidance only.
- OPERA ray azimuths from `how/startazA` per-ray arrays, with scalar and uniform-from-north fallbacks. Fixed the 100+ degree rotation from misusing `a1gate` as an azimuth offset. Covered by a real DWD fixture.
- Echo render speed: cached gate geometry plus direct scanline raster. Pan rebuild went from ~1800 ms to ~50 ms with no visual change.
- CARTO basemap with per-user API key in Settings, Esri fallback when empty, custom XYZ template support.
- Rotation v4: span-ROT retired from the map, shear overlay draws validated shear-watch candidates. Quiet-day soup gone, TVS/MESO untouched.
- Linux AppDir release from tags, WSL-verified build plus tests plus smoke run.
- AGPL-3.0-or-later, CPack wiring, About dialog credit.
- Velocity controls hide unless the active panel is VEL.
- Core derived products (Echo Tops, VIL), archive load, placefiles, spill-to-disk, lightning-jump engine, OPERA sessions, installer, a11y passes.
- Core derived products (Echo Tops, VIL), archive load, placefiles, spill-to-disk, lightning-jump engine, OPERA sessions, installer, a11y passes.
