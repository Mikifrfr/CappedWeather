# CappedWeather TODO

Short active list. Done work stays in git history and release notes, not in this file.

## Open

### Analysis depth
- [ ] Hail sizing / MEHS from VIL. Only ship when validated against labeled archives. No faked confidence.
- [ ] OPERA dual-pol unlock when 2027 moments land (CC, ZDR, PHI, KDP). Work is decoder mapping in `readOdimPvol` plus a fixture and availability tests. Per-panel auto-disable already handles their absence, so do not fake dual-pol from single-pol before then.
- [ ] Scan diagnostics full panel. The dock readout (`Scan: ...`) is live; the per-tilt table pop-out is still follow-on.
- [ ] Spill-to-disk prefetch. Scrubbing into spilled frames works but hitches on a volume-sized disk read. Add background prefetch.
- [ ] Export follow-on: ffmpeg-piped WebM stays documented but unwired. GIF is frame-exact, MP4 is window capture.

### Platform and UX
- [ ] macOS build. Windows installer (NSIS + ZIP) and Linux AppDir TGZ are shipped from tags. macOS is the remaining stretch.
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
- OPERA ray azimuths from `how/startazA` per-ray arrays, with scalar and uniform-from-north fallbacks. Fixed the 100+ degree rotation from misusing `a1gate` as an azimuth offset. Covered by a real DWD fixture.
- Echo render speed: cached gate geometry plus direct scanline raster. Pan rebuild went from ~1800 ms to ~50 ms with no visual change.
- CARTO basemap with per-user API key in Settings, Esri fallback when empty, custom XYZ template support.
- Rotation v4: span-ROT retired from the map, shear overlay draws validated shear-watch candidates. Quiet-day soup gone, TVS/MESO untouched.
- Linux AppDir release from tags, WSL-verified build plus tests plus smoke run.
- AGPL-3.0-or-later, CPack wiring, About dialog credit.
- Velocity controls hide unless the active panel is VEL.
- Core derived products (Echo Tops, VIL), archive load, placefiles, spill-to-disk, lightning-jump engine, OPERA sessions, installer, a11y passes.
