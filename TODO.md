# CappedWeather TODO

Short active list. Done work stays in git history and release notes, not in this file.

## Open

### Analysis depth
- [ ] Rainfall accumulation (one-hour and storm-total style) from Z-R over spill-to-disk history. Needs AP/clutter guard plus validation before any trust. No precision claims until then.
- [ ] OPERA dual-pol unlock when 2027 moments land (CC, ZDR, PHI, KDP). Work is decoder mapping in `readOdimPvol` plus a fixture and availability tests. Per-panel auto-disable already handles their absence, so do not fake dual-pol from single-pol before then.

### Overlays and context
- [ ] MTG Lightning Imager for European sessions. GLM covers GOES-18/19 only, OPERA sites stay dark without it.

### Playback and export
- [ ] Case-pack export/import. Zip of loaded volumes plus placefiles plus timeline state, so a shared case reproduces exactly.
- [ ] GIS export follow-on: GeoTIFF for one frame (needs a native TIFF dependency on every platform plus CI wiring). KML GroundOverlay is wired; uses current georeferencing only, no reprojection magic.

### Platform and UX
- [ ] macOS build. Windows installer (NSIS + ZIP) and Linux AppDir TGZ are shipped from tags. macOS is the remaining stretch.
- [ ] Flatpak plus Windows auto-update check. Artifacts already ship, discovery and updates are the gap.
- [ ] Offline basemap pack for field chasing. Pre-cached tile region with an explicit storage budget, so a no-signal chase still has roads.
- [ ] Light / high-contrast theme variant. Optional. Dark and Classic are complete including focus rules and dialog coverage.
- [ ] Card keyboard focus and full font-scale chrome. Map HUD scales with the text preference; dock titles, badges, and tabs are still theme-pinned.

### Research (explicitly not next)
- [ ] GPU volume rendering of live Level II. Design in `docs/gpu-volume-rendering.md` (shared VolumeBrick, GL raymarcher plus CPU twin). Not started.
- [ ] Storm-scale warn-on-forecast nowcasting. Motion extrapolation (+10/+20/+30 min) is done; ML needs infra plus labeled archives.
- [ ] Level III / national mosaic as optional context. Level II stays first.
- [ ] Broadcast / layout export packs. Out of scope unless product direction changes.

## Non-goals (for now)
- Full GR-style product scripting language
- Matching proprietary basemap imagery
- Becoming AWIPS or a TV automation suite
- Shipping synthetic or demo radar frames

## Recently done
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
