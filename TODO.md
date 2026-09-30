# CappedWeather — TODO

Source: product analysis vs GR2Analyst / peers + source-grounded UI audit (2026-07).
Guiding rule: close highest-value live-ops and chase gaps first. Do **not** become a GR2Analyst clone (skip full UDP engine / broadcast 3D as day-one goals).

---

## Done / nearly done

- [x] **Lightning-jump engine (GLM L2 + Level II fusion)** — open-source real-time 2σ LJA on GOES GLM, fused with NEXRAD. Spec: `docs/lightning-jump-engine.md`. Code + M5 verification exist; mark complete when netCDF production builds are the default path and TODO status is cleaned up.
- [x] **P1 first slice — alerts + right-click context menu** — `AlertCenter` sinks `WeatherOverlaySource::overlaysReady` and `GlmSource::cellsUpdated`, plays a sound (custom `.wav` via `QSoundEffect` or system beep fallback) and shows a tray balloon when a new NWS alert or a high-σ lightning jump is observed. Dedupe keyed by NWS `@id` and lightning cell id, with a configurable suppression window. `RadarMapCanvas::contextMenuEvent` exposes select / recenter / national view / zoom in/out / copy lat-lon / inspect / measure / cross-section. Both new behaviors gated by a `Qt6::Multimedia` opt-in (cleanly degrades to tray + beep otherwise).
- [x] **European OPERA radars as first-class sessions (v0.5.0)** — 179 operational OPERA sites in the picker/model/panels; ODIM_H5 volumes (`DBZH`→Reflectivity, `VRADH`→Velocity) stream over anonymous CloudFerro S3 via `OperaSource` and decode to the shared `DecodedRay` model, so tilts, timeline, Echo Tops/VIL, probing, and export work unmodified. HDF5-gated like netCDF. Ground truth + limits (24 h rolling cache only) in `docs/europe-sources.md`.

---

## P0 — Ship blockers / differentiator hygiene

- [x] **Require netCDF for production GLM builds** — document + default `CAPPEDWEATHER_REQUIRE_NETCDF=ON` for release; never ship a “lightning” checkbox that silently no-ops. Since v0.6.0 releases build `netcdf-c` + `hdf5` from vcpkg and bundle the DLLs, GLM ingestion and OPERA volumes are live out of the box.
- [x] **Surface GLM disabled state in UI** — when netCDF missing or GLM dead, disable lightning/fusion controls and show a clear status (not a quiet empty overlay).
- [x] **Mark LJA complete in docs** — flip this TODO + `docs/lightning-jump-engine.md` status once the above is true.
- [x] **First-run / empty-state guidance** — replace bare “Waiting for NOAA radar data” with a short workflow hint (pick station → LIVE → timeline → overlays).
- [x] **In-app keyboard shortcut reference** — Help menu or `?` overlay (Space, arrows, PgUp/Dn, 1–8, R, ±, L, etc.).

---

## P1 — Live ops & chase ergonomics (highest ROI vs GR2A without cloning it)

### Alerts & situational awareness
- [x] **Warning / jump audio + system notifications** — `AlertCenter` (sinks `WeatherOverlaySource::overlaysReady` + `GlmSource::cellsUpdated`) plays a sound (system beep fallback, or a user-selected `.wav` via `QSoundEffect` when `Qt6::Multimedia` is available) and shows a system tray balloon notification. Dedupe is keyed by NWS `@id` / lightning cell key with a configurable suppression window. Settings dialog has enable / sound / notification toggles, a geo filter around the selected radar or chase point, a comma-separated NWS event allowlist, lightning sigma threshold, suppression minutes, custom sound file picker, and a Test button.
- [x] **Radar health indicator** — small colored dot + label ("Online" / "Stale" / "Offline" / "Unknown") under the station summary. Updated on `volumeReady` (resets to Online when the latest frame is ≤ 5 min old), on `sourceFailed` (Offline), on radar selection (Stale until a new volume arrives), and reclassified every 30 s by a `healthTimer_`. Thresholds: ≤ 5 min Online, 5–15 min Stale, > 15 min Offline.
- [x] **CWA / selected-radar warning filter** — "Filter warnings to selected radar / chase point" checkbox + radius spinbox (10–1500 km) in the control dock. When enabled, `RadarMapCanvas::setProximityFilter` drops any warning whose exterior ring is outside the radius from the selected radar's coords (or the chase point when chase mode is on). Anchor is recomputed when the user picks a different radar or toggles chase mode in Settings.
- [x] **Minimum-severity alert filter** — `AlertConfig::minSeverity` (default = Severe) drops any warning whose NWS CAP `severity` tag is below the threshold before it reaches the sound + tray pipeline. Mapped from NWS taxonomy: 0=Minor, 1=Moderate, 2=Severe, 3=Extreme; Unknown is conservatively treated as Moderate. Exposed in Settings as a "Minimum severity" combo (Minor / Moderate / Severe (default) / Extreme). Persists under `alerts/minSeverity`. Stops the operator from being paged for Dense Fog Advisories, Air Quality Alerts, Special Weather Statements, etc. that previously fired on every NWS event.
- [x] **Multi pin cards** — `P` toggles a pin lock (`RadarMapCanvas::setPinnedOverlayLocked`); while locked, the active card survives any new overlay clicks. A small lock glyph is drawn in the card header so the user can see the state. The lock clears when the card is closed. Single-card replace remains the default for un-locked cards.
- [x] **Single-card replace is OK as default** — same TODO entry as "Multi pin cards"; the existing single-card replace behavior is preserved when the lock is off, and the lock is the user-opt-in escape hatch for keeping a card.
- [x] **Meteoalarm warnings for Europe** — EUMETNET visualization service (`visservice.meteoalarm.org`, same backend as meteoalarm.org): per-country hazard feeds joined to official warning-area polygons from vector tiles by area UUID, with per-area warning texts. Mapped onto the existing `WeatherWarning` pipeline (`WeatherOverlaySource` + map polygons + `AlertCenter` + severity/age/proximity filters) with no new alert machinery: awareness levels 1/2/3 map to Extreme/Severe/Moderate in official red/orange/yellow. Details and tile geometry cache per refresh round (bounded requests, most-severe-first).

### Map & tool UX
- [x] **Right-click map context menu** — `RadarMapCanvas::contextMenuEvent` exposes: Select radar (when over a station), Recenter on selected, National view, Zoom in/out, Copy lat/lon (formatted to 4 dp; clipboard copy to 6 dp), Inspect at cursor, and checkable Measure / Cross-section entries. The two tool toggles emit `measureModeToggled` / `crossSectionModeToggled` so the toolbar buttons stay in sync.
- [x] **Nav history undo/redo** — `RadarMapCanvas` keeps a bounded (50-entry) `viewBack_` / `viewForward_` ring of (center, zoom) snapshots. User-driven pan/zoom/setView on the active panel pushes to the back stack; the existing `synchronizingMapView_` guard prevents passive syncs from polluting history. `Ctrl+Z` pops back and pushes forward; `Ctrl+Shift+Z` / `Ctrl+Y` replay. Programmatic restores inside `undoView`/`redoView` set `suppressHistory_` so a redo doesn't immediately push the same view back. Status bar shows remaining history size after each operation.
- [x] **Rubber-band measure preview** — already in place: `mouseMoveEvent` updates `measureEnd_` whenever `measureStart_` is set in measure/cross-section mode, and `paintMeasurement()` draws the live distance/bearing line. `setCursor(Qt::CrossCursor)` is set in both `setMeasureMode()` and `setCrossSectionMode()`.
- [x] **Modeless cross-section window** — `MainWindow::showCrossSection` now opens a modeless `QDialog` (`WA_DeleteOnClose`) that follows the timeline via `refreshCrossSection()` on every `showFrame()`. Includes live timestamp in the title, CSV export with timestamped default name, and an explanatory caption. Live radar never blocks.
- [x] **Panel maximize from 2×2** — `togglePanelMaximized()` + `maximizePanelButton_` + View menu `F11` + double-click any panel. Temporary 1-panel focus; layout preset restored on toggle-off. `setPanelLayout()` exits maximize cleanly.
- [x] **Closable / floatable docks** — control + playback docks are `Movable|Closable|Floatable` with View-menu toggles synced to `visibilityChanged`, persisted via `saveState(1)`. Accessible names added so screen readers announce them.
- [x] **Named layout presets** — `presetsCombo_` next to the panel layout combo + a "Save..." button. Three built-in presets: "Chase 1x1" (1×1 REF), "Analysis 2x2 dual-pol" (2×2 REF/VEL/CC/ZDR), "Lightning scout 1x2" (1×2 REF/REF). User presets are stored under `presets/<name>` in QSettings with per-panel product + tilt, and are listed with a ★ prefix in the combo. Applying a preset sets the layout, switches each panel's product (without changing radar sessions), and activates the resulting panel.

### Overlays
- [x] **Per-panel overlay toggles** — new "Active panel overlays" section in the control dock with Warnings / Storm reports / GLM lightning / Radar fusion checkboxes plus a "Use global" reset. The four global checkboxes remain the default for every panel. Toggling any of the active-panel controls freezes the active panel's overlay state (`RadarMapCanvas::OverlayVisibility` stored as an `std::optional` on the canvas); the global toggles stop touching that panel until the user clicks "Use global".
- [x] **Overlay opacity + simple z-order** — `warningOpacitySlider_` (20–100%, persisted `view/warningOpacity`) scales warning fill/stroke alpha in `rebuildWeatherOverlay`; `warningsOnTopCheck_` (persisted `view/warningsOnTop`) flips paint order in `paintEvent` so echo can sit above polygons for debris-ball inspection.
- [x] **SPC report declutter** — `rebuildWeatherOverlay` thins hail/wind at zoom ≤ 6 to one marker per 14 px grid cell (tornadoes never thinned, hail outranks wind, tornado-first passes).

### Chase / field
- [x] **GPS / current-location chase** — "Detect from IP" button next to the chase latitude/longitude in the Settings dialog. One-shot `QNetworkAccessManager` GET to `https://ipwho.is/` (free, no key, JSON with `latitude`/`longitude`), 10 s timeout. Response is parsed and copied into the spinboxes; a confirmation dialog shows the approximate coords and reminds the user that the result is city-level, not GPS-grade. Coarse fallback for setups without a real GPS; a future revision can layer on Windows.Devices.Geolocation once the build environment is ready for it.
- [ ] **Touch / trackpad gestures** — pinch zoom, two-finger pan where Qt allows.
- [x] **Offline / degraded mode (banner slice)** — `offlineBanner_` top-center pill driven by consecutive `sourceFailed` vs `finalizeVolumeReady`; shows "OFFLINE — showing cached volumes + basemap" instead of a mysteriously empty map. Full tile-error wiring + spill-to-disk remains follow-on.
- [x] **Low-bandwidth preset** — `Data-saver` button in View: parallel chunks 2, 4-frame history, overlays every 2 min, with status-bar confirmation. One click for flaky cellular.
- [x] **Fast volume loads (progressive + zero-copy + cache)** — first pixels no longer wait for the full volume: partial ray snapshots paint at 500 rays then every 4000 (follow-latest only, `loading…` pill state), Echo Tops/VIL derive moved off the critical path (base publishes immediately, background copy derives and swaps in, ET/VIL chips enable on arrival), chunk handoff is `shared_ptr` (no per-chunk deep copy through queued delivery), 512 MB on-disk chunk cache per source (immutable S3 keys, listings always bypass), HTTP/2 multiplexing, 10 parallel chunks + 8-wide decode pools by default, OPERA listings at `max-keys=1000`. Honest limits: partials arrive in S3 order (patchy fill, not tilt-sorted); single-tilt volumes skip derive via `chunkSupportsDerivedProducts`.

### Playback & export
- [x] **Jump to earliest / latest + absolute time entry** — `Home` jumps to the oldest frame in the active session, `End` jumps to the newest (and re-engages LIVE follow when the LIVE toggle is on). A `QDateTimeEdit` with calendar popup sits next to the frame slider; editing it finds the closest loaded frame by `dataTime` and shows it.
- [x] **More loop speeds** — speed combo now offers 0.5×, 1×, 2×, 4×, 8×, 16×. Default is 2×. Legacy `timeline/speed` settings (0/1/2) are migrated to 1/2/3 so existing users' "2×" choice stays "2×".
- [x] **Cancellable GIF export with progress (non-blocking slice)** — `exportTimelineAnimation` now uses `QProgressDialog` with Cancel, restores the original frame on cancel/finish, timestamped CSV default for XS. Companion **Timeline MP4** (`exportTimelineMp4`): realtime window capture via `QWindowCapture` + platform recorder (MediaFoundation H.264 on Windows), same progress/cancel/restore choreography, graceful note when built without Qt Multimedia. Frame-exact output stays GIF's job; WebM/VP9 is not guaranteed by any platform backend — ffmpeg piping is the documented follow-on.
- [x] **Frame bookmarks** — filmstrip block per loaded volume with amber bookmark caps. `B` bookmarks the current frame, right-click toggles any block, persisted per radar under `bookmarks/<radarId>`. Warning-onset ticks mark *why* a frame matters.
- [x] **Tabbed sidebar + warning inbox + linked probe + command palette (UI surpass round)** — the 300-row scrolling dock is now Radar/Layers/Display/Alerts tabs with a pinned station card; the Alerts tab hosts a severity-sorted warning inbox (click = fly + pin via `RadarMapCanvas::focusWarning`, hover = accent outline via `paintWarningHighlight`); the playback dock hosts a `TimelineStrip` filmstrip; hovering one panel broadcasts `probeMoved(lonLat)` and every other visible panel samples its own product/volume into a dashed ghost chip (`sampleAtGeo` refactor of `updateProbe`, no cross-product contamination); `Ctrl+K` palette covers radars/products/layouts/actions. Supporting fixes: warning polygons get a dark-halo stroke so they survive bright velocity wash; the panel timestamp pill drops the 30-char locale timezone name for a short `local` suffix; double-clicking a pinned card toggles an expanded 520 px budget; storm trails extrapolate +10/+20/+30 min from a least-squares fit over the primary track's newest fixes. Deliberately deferred: light theme (full third sheet).

### Controls polish
- [ ] **Collapse / hide Velocity section when product ≠ VEL** — or show a one-line “switch to Velocity to enable”.
- [x] **Settings tooltips / help text** — added hover hints on the lightning σ threshold, σ window, cell size, min flash rate, and backfill settings. Combined with the existing tooltips across the rest of the dialog, every "obscure" knob now has a one-line explanation on hover. Palette import format example still belongs in the import file dialog/tooltip.
- [x] **Don’t cancel measure/XS silently on panel switch** — `activateMap` transfers the outgoing tool state to the new active panel (with button sync) instead of dropping it. `Esc` (canvas `keyPressEvent` + MainWindow shortcut) closes cards first, then cancels tools.
- [x] **First-run empty state + Help menu + HUD scale (v1.0 chase-ready slice)** — 3-step empty-state card, File/View/Help menubar (snapshot `Ctrl+S`, dock toggles, `F11` maximize, modeless `?`/`F1` help), scale bar + north arrow in `paintScaleBar`/`paintLegend`, `Esc`/`F11`/`Home`/`End`/`P` documented in help, larger 1440p/4K default window.
- [x] **UI/UX correctness pass (2026-09-29)** — a focused audit of `main_window.cpp`, `radar_map_canvas.cpp`, and `theme.cpp` turned up and fixed a set of defects that were only visible in the running app, not in the test suite:
  - **Dead layout** — `panelLayoutCombo_` was added to the View group layout twice, and a second `resizeDocks(272)` silently undid the 324 px the control dock needs for its two-column chip/layer rows. The Squall theme's 6 px map gutter was likewise overwritten 4 lines later by a hardcoded 4.
  - **Clipped product chips** — "Differential reflectivity" / "Vertically integrated liquid" were cut off mid-word in the two-column grid. Chips now carry the standard operational abbreviation (REF, VEL, SW, ZDR, RHOHV, PHIDP, ET18, VIL) with the full name + unit in a new `activeProductLabel` readout above the grid and in the tooltip.
  - **Canvas HUD collisions** — the status readout and the attribution overlapped on any panel narrower than 772 px (i.e. every panel in a 2×2 on a 1366×768 laptop); the station card completely covered the north arrow; the loading chip painted *over* the pinned detail card; probe and measure labels ran off the panel edge; the scale bar overflowed the panel past zoom 16 because the "nice distance" table started at 1 km; range-ring labels had no backing and vanished over bright echo; the legend had no fit guard and keyed report symbols that were switched off.
  - **Pinned card collapse** — a long NWS headline drove the body rect negative, so `setClipRect` blanked the whole card body and drew the scrollbar thumb outside its own track. The headline is now capped against the card budget with a fade marker, and the body rect is clamped. The severity badge was also measured at 11 pt and drawn at 8 pt (~17 px of dead pill per side).
  - **Dead zoom on high-resolution wheels** — `angleDelta().y() / 120` truncated to 0 for the fractional deltas (15/20/40) that precision trackpads and high-res wheels send, so map zoom did nothing at all on those devices. Normalized with a `pixelDelta()` fallback.
  - **Legend lied about the palette** — the swatch gradient was sampled with the user's echo opacity, so turning transparency up drove the legend toward black instead of showing the ramp.
  - **Hit testing disagreed with painting** — at zoom 5 the declutter rules were applied when drawing but not when hit-testing, so a hidden marker was still clickable. `radarAt` now mirrors the paint path.
  - **Context menu escape hatches** — right-click had no way to dismiss the detail card or cancel a tool, and "Inspect at cursor" over bare echo wrote the same transient text the hover uses (so the next mouse move erased it and the action looked broken). It now pins the sampled readout as a card, and a new `PinnedInfoKind::Probe` carries it.
  - **Performance** — the status readout was rebuilt from a four-level nested `.arg()` chain plus a `QDateTime` format on *every paint* (cached now); the ring set was heap-allocated and sorted per frame; `rebuildWeatherOverlay` deep-copied every warning polygon on the GUI thread even with both layers off; the spinner forced a full legend repaint 12.5×/s; `paintStationCard` did an O(N) `std::string`→`QString` scan per frame for one lookup that `siteIndexes_` already answers.
  - **Middle-click pan** and pointer/hand hover feedback over stations, with the measure/cross-section crosshair restored after a card drag (`unsetCursor()` reverted to the parent's arrow, not the tool cursor).
- [x] **Re-run the audit on the settings dialog** — flat 23-row form is now Display / Chase / Timeline / Network / Lightning / Map-cache / Alerts groups; fixed the `layout->addLayout(form)` re-parent anomaly and the doubled alerts-filter row. Added the Interface-text-scale preference below.


---

## P2 — Analysis depth (selected GR2A gaps worth stealing)

Prefer a **small set of high-signal products**, not a full user-defined-product language.

- [x] **Core derived products (2D), first slice** — Echo Tops (18 dBZ, kft MSL) + VIL (kg/m²) are materialized from every completed multi-tilt reflectivity volume, rendered/probed as first-class per-panel products, persisted in layouts, and covered by core + UI tests. Uses same-ground-range beam sampling, 4/3-earth geometry, an interpolated 18 dBZ crossing, and capped-56 dBZ VIL integration. Honest limits are documented in `docs/derived-products.md`; incremental incomplete-sweep updates and VILD/MEHS remain possible follow-ons rather than being faked here.
- [x] **Azimuthal shear / rotation tracker display (first-class overlay slice)** — `RadarMapCanvas::shearVisible_` + `paintShearOverlay()` renders `extractRadarStormFeatures()` objects independent of lightning. Rotating couplets get pink double-ring + `ROT dBZ | shear s^-1` label at zoom ≥ 6; strong non-rotating ≥ 50 dBZ cores get subtle gray dot at zoom ≥ 7. Fusion spokes stay under `lightningFusionVisible_`; fused features are skipped in the independent pass to avoid double-draw. Toggled by Layers `Rotation / shear` (`shearCheck_`, persisted `overlays/shear`), badge shows feature count / `off` / `idle`, legend adds `◎ ROT/shear` chip. `rebuildLightningFusion()` now always refreshes even when lightning hidden so shear survives with GLM off. Honest limits: thresholded display only (`minimumAzimuthalShear 0.0025 s^-1`, `minimumReflectivity 35 dBZ`), not an NWS RPG meso/TVS clone.
- [x] **Max-value trails (per-storm tracker)** — `StormTracker` (core, unit-tested) matches features across volumes by proximity (35 m/s gate, 2-miss coasting, scrub-safe updates), replacing the single-centroid trail whose global max jumped cells. Every storm draws its own fading line; the longest track keeps dots + `trail dBZ` + extrapolation. Per-track running gate maxima (`GateMaxTrail`) exist; per-pixel GR-style max-over-history remains follow-on.
- [x] **Deeper volume history with spill-to-disk** — limits raised as before, plus: evicted frames now spill to a versioned LE binary (`core/volume_spill`, Qt-free, round-trip tested) under the cache dir instead of being dropped; scrubbing faults them back synchronously and re-enforces the RAM budget. RAM window = `timeline/maximumFrames`, spilled tail capped at 8 volumes LRU. Derived-product swap re-spills when the frame is already out. Honest limits: scrub into spilled frames hitches on a volume-sized disk read; no background prefetch yet.
- [x] **Archive load (scoped)** — local Archive-II files/folders and selected-site UTC windows from the public `unidata-nexrad-level2` AWS bucket now decode off-thread into a dedicated 24-frame case timeline and the normal product pipeline. Includes bzip Archive-II framing, station/timestamp checks, bounded parallel downloads with atomic per-volume publication, cancellation/stale-local guards, endpoint-preserving volume sampling, real-public-object verification, and documented legacy/history limits in `docs/placefiles-and-archives.md`.
- [x] **Placefiles (read subset)** — local and refreshable HTTP(S) GR-compatible feeds now render `Place`, `Text`, `Icon`, `Line`, and `Polygon` features with stateful color/thresholds, click details, persisted credential-scrubbed URLs, last-known-good refresh behavior, parser/resource limits, and explicit unsupported sprite/object semantics in `docs/placefiles-and-archives.md`.
- [x] **Smoothing / gate display options (display-only slice)** — `RadarMapCanvas::displaySmoothing_` (default off, persisted `view/smoothing`, Layers `Smooth echo (display only)`). When on, `rebuildOverlay()` expands azimuth half-width ×1.35 so adjacent gates overlap and hide gaps; when off, honest native gate geometry. No data smoothing / no value interpolation — palette quantization (`round(normalized*63)/63`) unchanged.
- [x] **Scan diagnostics panel (dock-readout slice)** — `RadarMapCanvas::scanDiagnosticsText()` exposes `qualityText_ + scanQualityText_` from `analyzeScan()` (rays, coverage°, max gap°, max range). `MainWindow::scanDiagnosticsLabel_` under Layers shows `Scan: …` / `Scan: —`, refreshed in `refreshLayerBadges()` on every `showFrame()`. Full pop-out panel with per-tilt table remains follow-on.
- [x] **TVS / mesocyclone markers (first slice)** — `core/circulation_detector` (NSSL-style heuristic, explicitly NOT a port of the NWS RPG TDA) finds gate-to-gate azimuthal couplets on dealiased velocity with range-dependent delta-V gates, a 20 dBZ reflectivity guard, vertical association across 2+ tilts, and TVS vs meso classification by intensity/diameter (≤3.0 km)/depth (2500/3000 m). `RadarMapCanvas::persistCirculations` adds time continuity (2 hits to show, 2 misses to drop, 24-track cap) so single-volume flicker never draws; `paintCirculationMarkers` draws red-triangle TVS / orange-diamond MESO with delta-V + depth labels. Layers `TVS / meso markers` toggle (`tvsCheck_`, persisted `overlays/tvs`) with count/`off`/`idle` badge. Covered by synthetic-volume core tests (clear-air, single-tilt, NaN/folded, weak/speckle, shallow, misaligned, azimuth-gap vetoes). Treat markers as guidance, never warnings.
- [ ] **Hail / MEHS + RPG-fidelity follow-on** — VIL-based hail sizing and closer RPG TDA fidelity only when validated against labeled archives; do not fake confidence.
- [ ] **OPERA dual-pol unlock (gated on 2027 data)** — OPERA plans CC/ZDR/PHI/KDP moments from 2027 onwards. Work is decoder mapping in `readOdimPvol` (new quantities → existing products) plus fixture + availability tests; the per-panel auto-disable path already handles their absence. Do NOT fake dual-pol from single-pol moments before then.

---

## P3 — Platform, a11y, packaging

- [x] **One-click Windows installer** — NSIS via CPack (`ZIP;NSIS`), Start-menu shortcut, uninstall-before-install. Same payload as the portable ZIP (Qt + BZip2 + netCDF + HDF5). Shipped locally with `gh release` (no CI quota spent).
- [x] **Accessible names/descriptions (slice)** — canvas (`StrongFocus`, panel name + description), control/playback docks, measure/XS buttons, empty-state/offline banners, transport buttons with shortcut hints. `Esc` closes cards/cancels tools; `F11` maximizes. Full card keyboard-focus + font-scale remains follow-on.
- [x] **Visible keyboard focus + readable disabled state** — Qt stops drawing the default focus rectangle as soon as a QSS rule sets a `border`, and *neither* theme defined a `:focus` rule for any button, checkbox, slider, or menu, so Tab moved through the whole sidebar with zero feedback. Both themes now have focus rules, and the menu bar (which had no styling at all despite a menubar existing) is themed.
- [x] **Contrast pass on secondary text and disabled controls** — captions, group headers, layer sub-labels, the station-picker placeholder, and every disabled widget sat at ~2.8:1 / ~1.1:1 against the dock body, i.e. below WCAG AA and, for disabled controls, effectively invisible. Bumped to ≥4.5:1 for text and a clearly visible disabled border. The Classic theme's slider also gave `sub-page` and `add-page` the same color, so echo/warning opacity and the frame scrubber showed *no value at all*.
- [x] **Both themes cover the same widgets** — `QDateTimeEdit` (the playback dock's jump-to-time field) and `QGroupBox` (Settings → Alerts) were missing from the Classic sheet and rendered as native widgets inside a themed dialog; the shortcut-help dialog was pinned to Classic colors by an inline stylesheet. All moved into the sheets so a live theme switch is actually complete.
- [x] **Settings dialog scrolls** — 23 form rows plus the alerts group pushed OK/Cancel off-screen at non-default text sizes. Body is in a `QScrollArea` with the button box pinned.
- [x] **Status bar elides instead of hard-cutting** — the three-part status line was clipped mid-word next to the permanent widgets; it now elides against the live width (recomputed on status-bar resize) with the full text in the tooltip.
- [x] **Font scale preference** — Settings → Display → Interface text scale (80–150%, live, persisted `ui/fontScale`). Scales every map HUD painter via `setHudFontScale` plus the app font. Theme-pinned chrome (dock titles, badges, tabs) stays fixed — documented in the row tooltip.
- [ ] **Color-safe palette preview** — show swatch in selector; consider offering as recommended default option.
- [ ] **Light / high-contrast theme variant** (optional).
- [ ] **Enforce restart-required settings** or apply cache limits live without lying in labels.
- [x] **Default window sizing on large displays** — 1440p/4K uses 90%/88% of available geometry (up to 1800×1050); smaller screens keep the old compact default. Persisted geometry still wins when present.
- [ ] **Cross-platform stretch** — Linux/macOS builds once Windows installer path is solid (Qt already helps).

---

## P4 — Research / long-horizon (keep explicit, don’t pretend they’re next sprint)

- [ ] **GPU volume rendering of live Level II** — ray-marched 3D dual-pol at interactive rates. GR2A does 3D closed/paid; open world has nothing good. Novel, pure GPU pain. Only after P0–P2 live-ops gaps.
- [ ] **Storm-scale severe nowcasting** — 0–60 min meso tracks / az-shear (budget warn-on-forecast). **Not** precip nowcasting. Motion-based track prediction/extrapolation is done (`StormTracker` velocity + top-3 +10/+20/+30 min); ML warn-on-forecast needs ML infra + labeled archives. Highest impact, highest risk.
- [ ] **Level III / national mosaic layer** — optional context under Level II (not a pivot away from L2-first).
- [ ] **Broadcast / layout export packs** — out of scope unless product direction changes.

---

## Explicit non-goals (for now)

- Full GR2Analyst user-defined product scripting language  
- Matching GR proprietary basemap imagery  
- Becoming AWIPS or a TV automation suite  
- Shipping synthetic/demo radar frames  

---

## Suggested sequencing

1. **P0** — GLM honesty + empty-state + shortcuts (days)  
2. **P1 alerts + map tools + overlay opacity/per-panel** — live-ops feel (1–2 milestones)  
3. **P1 chase GPS/offline + export** — field reliability  
4. **P2 Echo Tops/VIL + placefiles + scoped archive** — analysis credibility without 3D  
5. **P3 installer** — distribution  
6. **P4 3D / nowcast** — research tracks only when the live console is sticky  

---

## Reference gaps (from competitive audit)

| Gap | Why it matters | Priority |
|-----|----------------|----------|
| Silent GLM without netCDF | Differentiator can look broken | P0 |
| No warning alerts | Ops safety / chase attention | P1 |
| No placefiles | Chaser ecosystem staple | P2 |
| No derived ET/VIL/MEHS | Table-stakes severe analysis | P2 |
| No archive | Case study / post-event | P2 (scoped) |
| No 3D volume | GR2A crown jewel | P4 |
| No UDP engine | GR power-user niche | Non-goal |
| Modal cross-section | Blocks live radar | P1 |
| Global-only overlays | Hurts 2×2 workflows | P1 |
| No GPS chase | Manual lat/lon friction | P1 |
| GIF-only / UI-blocking export | Sharing + UX hitch | P1 |
| Thin discoverability | New user drop-off | P0–P1 |
