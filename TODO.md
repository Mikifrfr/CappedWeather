# CappedWeather TODO

Short active list. Done work stays in git history and release notes, not in this file.

## Open

### Analysis depth
- [ ] Rainfall accumulation (one-hour and storm-total style) from Z-R over spill-to-disk history. Needs AP/clutter guard plus validation before any trust. No precision claims until then.
- [ ] OPERA dual-pol unlock when 2027 moments land (CC, ZDR, PHI, KDP). Work is decoder mapping in `readOdimPvol` plus a fixture and availability tests. Per-panel auto-disable already handles their absence, so do not fake dual-pol from single-pol before then.

### Overlays and context
- [ ] MTG Lightning Imager for European sessions. GLM covers GOES-18/19 only: MTG LI L2 lives behind EUMETSAT Data Store registration plus OAuth (no anonymous feed like NOAA S3), so this needs a user-supplied credential flow first. No code until then.

### Playback and export
- [ ] Case-pack export/import. Zip of loaded volumes plus placefiles plus timeline state, so a shared case reproduces exactly.
- [ ] GIS export follow-on: GeoTIFF for one frame (needs a native TIFF dependency on every platform plus CI wiring). KML GroundOverlay is wired; uses current georeferencing only, no reprojection magic.

### Platform and UX
- [ ] macOS build. Windows installer (NSIS + ZIP) and Linux AppDir TGZ are shipped from tags. macOS is the remaining stretch.
- [ ] Flatpak plus Windows auto-update check. Artifacts already ship, discovery and updates are the gap.
- [ ] Light / high-contrast theme variant. Optional. Dark and Classic are complete including focus rules and dialog coverage.

### Research (explicitly not next)
- [ ] GPU volume rendering of live Level II. Design in `docs/gpu-volume-rendering.md` (shared VolumeBrick, GL raymarcher plus CPU twin). Not started.
- [ ] Storm-scale warn-on-forecast nowcasting. Motion extrapolation (+10/+20/+30 min) is done; ML needs infra plus labeled archives.
- [ ] Level III / national mosaic as optional context. Level II stays first.
- [ ] Broadcast / layout export packs. Out of scope unless product direction changes.

## Proposed (researched, not committed)

Ready to build (data live now, testable hermetically plus replay):
- [ ] Dual-pol QPE for NEXRAD. R(Z,ZDR)/R(KDP) accumulations next to Z-R over spill history, same AP/clutter guard requirement as the rainfall item. No trust until gauge/MRMS comparison.
- [ ] TVS/meso markers into AlertCenter. Opt-in sink like warnings and lightning, always labeled guidance, honoring severity and suppression settings. Verify: hermetic ingest tests.
- [ ] Hail swaths from tracks plus cores. Per-storm polygons accumulate along StormTracker paths where hail tiers persist, exportable like warnings. Verify: synthetic tracks plus case replays.
- [ ] Precip-type map. Bright-band height crossed with METAR surface temps: rain/snow/mix shading where the low levels are sampled. Winter-only value, experimental label until scored.

Research-gated (confirm open shape first, METAR/ECCC style):
- [ ] NWS damage-assessment (DAT) tornado paths. Post-event survey polygons as an overlay layer; confirm the open endpoint shape first.
- [ ] GOES satellite overlay. HRV/IR from the anonymous NOAA S3 buckets through the optional netCDF reader, remapped to the panel grid. Confirm product keys and sizes first.
- [ ] HRRR simulated reflectivity. NOMADS or TDS NetCDF subset as a model-context overlay with valid-time labels, never mixed with observed echo. Confirm subset API first.
- [ ] Observed soundings plus hodograph. IEM parsed-sounding JSON for 00Z/12Z sites, Skew-T-lite or hodograph pop-out feeding SRH ground truth. Confirm feed shape first.
- [ ] NHC cyclone plus cone overlay. Open NHC GIS for active storms, same polygon machinery as warnings, seasonal value. Confirm endpoint first.
- [ ] HMS fire/smoke overlay. NOAA open smoke/fire polygons for western sessions, same machinery as warnings. Confirm endpoint first.
- [ ] WPC excessive-rainfall outlooks. Same SPC-style GeoJSON pattern from another office; confirm file layout first.
- [ ] Nearest-radar mosaic. Composite of adjacent loaded sessions at matched tilts as optional context, Level II first per the research item. Prototype on two synced radars, measure seams before committing.

## Non-goals (for now)
- Full GR-style product scripting language
- Matching proprietary basemap imagery
- Becoming AWIPS or a TV automation suite
- Shipping synthetic or demo radar frames

## Recently done
- Storm-relative helicity 0-1/0-3 km from VAD plus Bunkers motion (pair-sum integration with per-layer span gates, scan pop-out readout with quality flags, closed-form core tests, documented in derived-products).
- Bunkers storm motion from VAD (0-6 km gateCount-weighted mean wind with 7.5 m/s shear-perpendicular split, weak-shear flag, VAD-style quality, right-mover line in the scan pop-out, closed-form core tests).
- TBSS hail-spike detector (60+ dBZ core with 4-30 km down-radial weak-echo spike on 3+ adjacent rays, map layer with badge and persisted toggle, six-case core tests).
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
