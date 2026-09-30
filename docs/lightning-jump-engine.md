# Lightning-Jump Engine — Design Spec

Status: production (M1–M5 complete; netCDF required for production builds per P0.1)
Owner: miki
Started: 2026-07-20

## What and why

A lightning jump is a rapid surge in a storm's **total flash rate** (in-cloud +
cloud-to-ground). It signals updraft intensification and precedes severe weather —
documented lead times of ~12–22 min before tornadoes in case studies, and NWS
forecasters have issued warnings primarily on jumps. The reference implementation
(Schultz/Chronis LJA, feeding ProbSevere) lives inside NASA/NOAA pipelines.
**There is no open-source, real-time, publicly usable implementation.** This engine
is that implementation, built into CappedWeather.

Goal: real-time CONUS lightning-jump detection with sigma-level intensity, overlaid
on the radar map, later fused with Level II mesocyclone/azimuthal-shear analysis.

## Algorithm (Lightning Jump Algorithm, LJA)

References: Schultz et al. 2009/2011; Chronis et al. 2015 (Wea. Forecasting 30,
23–37); GOES-R GLM operational LJA.

Per storm region (phase 1: lat/lon grid cell; later: tracked storm cell):

1. **Flash rate** FR(t) = total flashes per minute, binned at `rateResolution`
   (default 60 s).
2. **DFRDT(t)** = (FR(t) − FR(t−1)) / Δt  [flashes min⁻²].
3. **σ(t)** = sample stddev of DFRDT over the trailing `sigmaWindow`
   (default 12 min), excluding the current DFRDT sample so the jump itself does
   not inflate σ. Requires ≥ `minSigmaSamples` (default 5), else "insufficient data".
4. **Jump** when `DFRDT ≥ 2σ` **and** `FR(t) ≥ minFlashRate` (default 10 fl/min —
   kills false jumps at noise-level rates).
5. **Sigma level** = DFRDT/σ, bucketed 2σ/3σ/4σ/5σ+ for display intensity.
6. Jump stays **active** while DFRDT ≥ 0; **jump cessation** (DFRDT < 0 after an
   active jump) is emitted as its own event — cessation clustering near tornado
   time is documented in the literature and is itself a signal.
7. σ ≈ 0 (flatline history) → no jump, conservative. Documented deviation from
   some descriptions of the operational algorithm; avoids div-by-zero false positives.

All knobs live in `LightningJumpConfig` — the literature has resolution/window
variants (1-min vs 2-min rates, 12-min vs 14-min windows), so nothing is hardcoded.

## Data sources

### GOES GLM Level 2 LCFA (primary)

- Buckets (AWS Open Data, anonymous, `us-east-1`):
  `s3://noaa-goes19/GLM-L2-LCFA/YYYY/DOY/HH/` (GOES-East, operational since 2025-04)
  `s3://noaa-goes18/GLM-L2-LCFA/YYYY/DOY/HH/` (GOES-West)
- netCDF-4 files, ~20 s cadence: `OR_GLM-L2-LCFA_G19_sYYYYJJJHHMMSS0_..._.nc`
- Vars used: `flash_id`, `flash_time_offset_of_first_event` (s, relative to
  `product_time`, epoch 2000-01-01T12:00:00Z), `flash_lat`, `flash_lon`,
  `flash_energy` (J), `flash_area` (km²). Read units attrs defensively.
- Real-time latency ~1–3 min. Listing via unsigned `?list-type=2&prefix=` GETs —
  same pattern as the existing NEXRAD `s3_object` code.

### NEXRAD Level II (fusion, M4+)

Already in the app. Fusion plan: correlate jump polygons with mesocyclone /
azimuthal-shear features; verification stats vs NWS warning archive (IEM API).

## Architecture

```
cappedweather_core (pure C++, testable)
├── lightning_flash.hpp      flash/event types
├── flash_rate_grid.*        lat/lon binning → per-cell FlashRateSeries
├── lightning_jump.*         LJA core: DFRDT, running σ, jump state machine
└── glm_l2_reader.*          netCDF LCFA parser (optional, netCDF-guarded)
                             + S3 key helpers (netCDF-free, unit-tested)

app layer (Qt, later milestones)
├── glm_source.*             S3 poller for GLM buckets (mirrors nexrad_source)
└── map overlay              jump cells colored by sigma level, cessation markers
```

Dependency: netCDF-C (vcpkg `netcdf-c`, or system). Guarded by
`CAPPEDWEATHER_HAS_NETCDF`; core lib and tests build without it.

## Milestones

- **M1** — core math: flash types, rate grid, LJA detector, unit tests. ✅ 2026-07-20
- **M2** — GLM L2 reader + local-file replay tool (feed archived files from disk,
  validate flash extraction vs known case days). ✅ 2026-07-20 — see
  `docs/validation-2025-05-16.md` (62% POD, 8 min median lead on the 2025-05-16
  outbreak, zero tuning). Note: netCDF-C via a local conda env / vcpkg.
- **M3** — live S3 poller in app; jump cells on the map with sigma-level colors. ✅ 2026-07-23 (`GlmSource` S3 poller, `radar_map_canvas` jump overlay, GLM intelligence inspector).
- **M4** — Level II fusion: jump ↔ mesocyclone/az-shear correlation view. ✅ `radar_lightning_fusion` correlates jumps with NEXRAD reflectivity objects; surfaced in the GLM intelligence panel.
- **M5** — verification harness: archived outbreak days, jump lead time vs NWS
  warnings (IEM archive), POD/FAR stats. Tune `minFlashRate` + window from data. ✅ `data/verification/results/m5-report.md` (POD 0.65–0.76, FAR 0.69–0.81, CSI 0.18–0.26 on 2025-05-16 midwest + 2025-05-18 kansas cases, 397 warnings).

## Risks / honest notes

- GLM covers the Americas — fine for NEXRAD CONUS fusion, useless for Europe.
  (EU side later: MTG LI, first data expected ~2026, or ground networks.)
- Grid-based (no storm tracking) jumps can smear across fast-moving cells; M4
  mitigates with radar-feature association. Accepted for phase 1.
- GLM flash clustering is done upstream by the LCFA algorithm — we trust
  `flash_id`, we do NOT re-cluster events into flashes ourselves.
- 20 s file cadence vs 60 s rate bins: fine, flashes accumulate.
