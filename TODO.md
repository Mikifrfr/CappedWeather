# CappedWeather — TODO

Source: product analysis vs GR2Analyst / peers + source-grounded UI audit (2026-07).
Guiding rule: close highest-value live-ops and chase gaps first. Do **not** become a GR2Analyst clone (skip full UDP engine / broadcast 3D as day-one goals).

---

## Done / nearly done

- [x] **Lightning-jump engine (GLM L2 + Level II fusion)** — open-source real-time 2σ LJA on GOES GLM, fused with NEXRAD. Spec: `docs/lightning-jump-engine.md`. Code + M5 verification exist; mark complete when netCDF production builds are the default path and TODO status is cleaned up.
- [x] **P1 first slice — alerts + right-click context menu** — `AlertCenter` sinks `WeatherOverlaySource::overlaysReady` and `GlmSource::cellsUpdated`, plays a sound (custom `.wav` via `QSoundEffect` or system beep fallback) and shows a tray balloon when a new NWS alert or a high-σ lightning jump is observed. Dedupe keyed by NWS `@id` and lightning cell id, with a configurable suppression window. `RadarMapCanvas::contextMenuEvent` exposes select / recenter / national view / zoom in/out / copy lat-lon / inspect / measure / cross-section. Both new behaviors gated by a `Qt6::Multimedia` opt-in (cleanly degrades to tray + beep otherwise).

---

## P0 — Ship blockers / differentiator hygiene

- [x] **Require netCDF for production GLM builds** — document + default `CAPPEDWEATHER_REQUIRE_NETCDF=ON` for release; never ship a “lightning” checkbox that silently no-ops.
- [x] **Surface GLM disabled state in UI** — when netCDF missing or GLM dead, disable lightning/fusion controls and show a clear status (not a quiet empty overlay).
- [x] **Mark LJA complete in docs** — flip this TODO + `docs/lightning-jump-engine.md` status once the above is true.
- [x] **First-run / empty-state guidance** — replace bare “Waiting for NOAA radar data” with a short workflow hint (pick station → LIVE → timeline → overlays).
- [x] **In-app keyboard shortcut reference** — Help menu or `?` overlay (Space, arrows, PgUp/Dn, 1–6, R, ±, L, etc.).

---

## P1 — Live ops & chase ergonomics (highest ROI vs GR2A without cloning it)

### Alerts & situational awareness
- [x] **Warning / jump audio + system notifications** — `AlertCenter` (sinks `WeatherOverlaySource::overlaysReady` + `GlmSource::cellsUpdated`) plays a sound (system beep fallback, or a user-selected `.wav` via `QSoundEffect` when `Qt6::Multimedia` is available) and shows a system tray balloon notification. Dedupe is keyed by NWS `@id` / lightning cell key with a configurable suppression window. Settings dialog has enable / sound / notification toggles, a geo filter around the selected radar or chase point, a comma-separated NWS event allowlist, lightning sigma threshold, suppression minutes, custom sound file picker, and a Test button.
- [ ] **Radar health indicator** — clear online / stale / offline affordance (not only status text).
- [ ] **CWA / selected-radar warning filter** — reduce national noise; “warnings near active radar / chase point” mode.
- [ ] **Multi pin cards** — pin more than one warning/report/lightning card at once for comparison.

### Map & tool UX
- [x] **Right-click map context menu** — `RadarMapCanvas::contextMenuEvent` exposes: Select radar (when over a station), Recenter on selected, National view, Zoom in/out, Copy lat/lon (formatted to 4 dp; clipboard copy to 6 dp), Inspect at cursor, and checkable Measure / Cross-section entries. The two tool toggles emit `measureModeToggled` / `crossSectionModeToggled` so the toolbar buttons stay in sync.
- [ ] **Nav history undo/redo** — pan/zoom stack (misclick recovery).
- [ ] **Rubber-band measure preview** — live range/bearing line while placing the second point; crosshair cursor in measure/XS modes.
- [ ] **Modeless cross-section window** — replace modal `QDialog::exec()` so live radar stays usable beside the XS view.
- [ ] **Panel maximize from 2×2** — temporary full-screen one panel without nuking the whole layout preset.
- [ ] **Closable / floatable docks** — allow hiding control/playback docks to maximize map (persist layout).
- [ ] **Named layout presets** — e.g. “Chase 1×1”, “Analysis 2×2 dual-pol”, restore docks + panel products in one click.

### Overlays
- [ ] **Per-panel overlay toggles** — warnings / reports / lightning / fusion independent per panel.
- [ ] **Overlay opacity + simple z-order** — especially warning polygons vs radar echo.
- [ ] **SPC report declutter** — collision avoidance / density control in outbreak clusters.
- [ ] **Single-card replace is OK as default** — but support “pin” lock so a card isn’t wiped by the next click.

### Chase / field
- [ ] **GPS / current-location chase** — “Use current location” (Windows location API or NMEA/GPSD later); periodic nearest-radar refresh.
- [ ] **Touch / trackpad gestures** — pinch zoom, two-finger pan where Qt allows.
- [ ] **Offline / degraded mode** — keep last volumes + disk-cached basemap tiles usable when cellular dies; clear “offline” banner.
- [ ] **Low-bandwidth preset** — one-click: parallel chunks=1–2, shorter history, overlays throttled.

### Playback & export
- [ ] **Jump to earliest / latest + absolute time entry** — type a UTC/local time; Home/End style shortcuts.
- [ ] **More loop speeds** — 0.5×, 8×, 16× (and optional custom interval).
- [ ] **Non-blocking GIF/MP4 export** — worker thread; add MP4/WebM for chaser sharing (GIF kept).
- [ ] **Frame bookmarks** — mark notable volumes in the short timeline and jump back.

### Controls polish
- [ ] **Collapse / hide Velocity section when product ≠ VEL** — or show a one-line “switch to Velocity to enable”.
- [ ] **Settings tooltips / help text** — especially lightning σ window, cell size, GLM knobs; palette import format example in the file dialog/tooltip.
- [ ] **Don’t cancel measure/XS silently on panel switch** — confirm, transfer, or keep tool state.

---

## P2 — Analysis depth (selected GR2A gaps worth stealing)

Prefer a **small set of high-signal products**, not a full user-defined-product language.

- [ ] **Core derived products (2D)** — start with Echo Tops + VIL (then VILD / MEHS if solid). Update live as tilts arrive when possible.
- [ ] **Azimuthal shear / rotation tracker display** — surface existing fusion shear ideas as a first-class product or overlay (path toward meso awareness without full NEXRAD RPG clone).
- [ ] **Max-value trails (simple)** — trail of max REF or ZDR over loaded history (poor-man’s GR max trails).
- [ ] **Deeper in-memory / on-disk volume history** — raise practical loop depth; optional spill-to-disk for long events without full archive UI.
- [ ] **Archive load (scoped)** — open local Level II file/folder **or** fetch a time window from public AWS archive for one site/date. Case-study mode, not a full GR archive browser on day one.
- [ ] **Placefiles (read subset)** — GR-compatible placefile fetch/render for chaser dots, boundaries, spotter networks (biggest chaser ecosystem gap).
- [ ] **Smoothing / gate display options** — optional display smoothing; below-threshold already exists — keep gates honest by default.
- [ ] **Scan diagnostics panel** — promote `analyzeScan()` coverage/gap/range into a readable dock readout.
- [ ] **NEXRAD algorithm markers (optional later)** — TVS/meso/hail only if quality bar is real; do not fake confidence.

---

## P3 — Platform, a11y, packaging

- [ ] **One-click Windows installer** — Qt + BZip2 + netCDF bundled; no “build the weather app” onboarding.
- [ ] **Accessible names/descriptions** — canvas, docks, HUD, cards; Escape closes cards; keyboard focus for pins.
- [ ] **Font scale preference** — HUD/legend minimum sizes configurable for chase laptops.
- [ ] **Color-safe palette preview** — show swatch in selector; consider offering as recommended default option.
- [ ] **Light / high-contrast theme variant** (optional).
- [ ] **Enforce restart-required settings** or apply cache limits live without lying in labels.
- [ ] **Default window sizing on large displays** — use more of 1440p/4K real estate.
- [ ] **Cross-platform stretch** — Linux/macOS builds once Windows installer path is solid (Qt already helps).

---

## P4 — Research / long-horizon (keep explicit, don’t pretend they’re next sprint)

- [ ] **GPU volume rendering of live Level II** — ray-marched 3D dual-pol at interactive rates. GR2A does 3D closed/paid; open world has nothing good. Novel, pure GPU pain. Only after P0–P2 live-ops gaps.
- [ ] **Storm-scale severe nowcasting** — 0–60 min meso tracks / az-shear (budget warn-on-forecast). **Not** precip nowcasting. Needs ML infra + labeled archives. Highest impact, highest risk.
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
