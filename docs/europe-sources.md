# Europe: OPERA/ODIM volumes — verified ground truth + plan

All endpoints below were probed live on 2026-09-05. No API keys were used.
User direction: verify access first, skip the composite-layer detour, go
straight at ODIM polar volumes.

## What is open (verified)

### 1. OPERA single-site volumes on anonymous S3 — the primary path

- Bucket `openradar-24h` on CloudFerro S3, anonymous List + Get:
  `https://s3.waw3-1.cloudferro.com/openradar-24h?list-type=2&prefix=...`
- Layout: `YYYY/MM/DD/CC/site/PVOL/`, e.g. `2026/09/05/NO/nohur/PVOL/`
- Object naming: `<site>@<YYYYMMDDTHHMM>@<el1>_<el2>_...@<MOMENT>.h5`
  e.g. `nohur@20260905T0000@0.5_1.0_1.6_2.4_3.2_4.2_5.4_6.8_8.5_10.4_12.8_15.5@DBZH.h5`
  (~0.5–2 MB). The filename itself lists every elevation: tilt discovery
  without downloading.
- One object = one moment × N elevations. A "volume" is a single GET
  (compare: NEXRAD needs ~70 chunk GETs). Moments ride separate objects
  on their own schedules (DBZH/TH share a scan table, VRADH interleaves
  its own, e.g. `...T0006@2.6_5.2_..._90.0@VRADH.h5`).
- Moments available now: DBZH (QC reflectivity), TH (unfiltered
  reflectivity), VRADH (radial velocity). Dual-pol (CC, ZDR, PHI, KDP)
  is announced for 2027+. This maps exactly onto the existing TDWR
  pattern: three base moments, dual-pol auto-disabled.
- Rolling 24 h cache only. There is no European equivalent of the AWS
  Level II archive on this bucket (long-term archive "2012–, TBD" is
  API-only). Timeline depth for Europe is limited to the last day.
- Format: ODIM_H5 polar volumes (HDF5). Requires an HDF5 reader
  (vcpkg `hdf5`, same REQUIRE-gate pattern as netCDF).

### 2. ORD API (MeteoGate) — catalog + metadata, keyed for quota

- `https://api.meteogate.eu/eu-eumetnet-weather-radar`, OGC EDR style.
  Anonymous queries are allowed but throttled hard (consistent HTTP 429
  in probing). Free keys at `https://devportal.meteogate.eu/`.
- Useful without a key: nothing reliable. Useful WITH a key: station
  search (`collections/observations/locations`), per-elevation SCAN
  metadata (nbins/nrays/rscale/elangle without downloading).
- MQTT notifications (`username: everyone`) exist but we do not need
  them; polling S3 listings on the overlay cadence is enough.
- Station catalog source of truth:
  `EUMETNET/openradardata-validator/.../stations/OPERA_RADARS.csv`
  (220 radar rows with lat/lon, ODIM code, WIGOS id, band, max range).
  The S3 country segment is the uppercased first two letters of the
  ODIM code (`nohur` → `NO`, `deess` → `DE`, `bezav` → `BE`).

### 3. DWD open data — fallback / Germany depth (probed live, no key)

- `https://opendata.dwd.de/weather/radar/composite/` → `wn/ pg/ rv/
  vii/ rs/ hg/ hx/ dmax/ hymecng/`, 5-minute `WN<date>.tar.bz2` files.
- `.../radolan/`, `.../sites/`, `.../mesocyclones/`, `.../konrad3d/`
  also live. Formats are DWD-specific (BUFR, RADOLAN binary,
  run-length products) — NOT ODIM. Only worth it if OPERA misses a
  German site or for mesocyclone markers later.

### 4. Licensing

- OPERA data: CC BY 4.0. The app must credit EUMETNET OPERA wherever
  European data renders (map chip + docs + release notes). Same class
  of requirement as the Esri/OSM attribution already painted on-canvas.

### Explicitly out

- OPERA NIMBUS composites as a map layer (user: skip the detour).
- KNMI/SMHI/FMI national APIs (per-service keys, redundant once OPERA
  volumes flow; revisit only for sites OPERA lacks).
- Meteoalarm warnings: natural follow-up onto the WeatherWarning
  pipeline, not this phase.
- OPERA BUFR volumes: HDF5 covers everything recent; do not write a
  BUFR decoder.

## Implementation plan: ODIM volumes as first-class radar sessions

Goal: a German/Dutch/Norwegian site behaves exactly like a US one —
pick station, LIVE follow, tilts, products, timeline, cross-sections.

### Core: `src/core/odim_reader.{hpp,cpp}` + tests

- Parse ODIM_H5 PVOL with HDF5-C: `/datasetN/{dataM/data,what/quantity}`,
  `/datasetN/where/elangle,nbins,nrays,rstart,rscale`,
  `data{what/gain,offset,nodata,undetect}`.
- Map moments: DBZH → Reflectivity, VRADH → Velocity, TH → (hold;
  expose as Reflectivity only if DBZH absent, else ignore — decide in
  review; never invent a ninth product silently).
- Emit the existing `DecodedRay` model: one ray per azimuth per
  elevation, gate values decoded with gain/offset, nodata/undetect
  mapped to the existing below-threshold/missing sentinels.
- Elevation numbers: assign sequentially sorted by elangle (ODIM has
  no stable tilt numbering across sites).
- Unit tests: hand-built minimal ODIM_H5? netCDF-style stub problem —
  HDF5 has no hand-rolled writer. Options: (a) commit a tiny real
  fixture file (~100 KB, one elevation, CC BY 4.0, attributed);
  (b) generate the fixture at test time with the HDF5 lib when
  available, skip otherwise. Prefer (a): deterministic, tests the
  reader exactly as shipped.
- Build gating: `CAPPEDWEATHER_REQUIRE_HDF5` default ON for production
  (same fail-fast philosophy as netCDF), OFF for stub CI.

### App: `src/app/opera_source.{hpp,cpp}`

- Mirrors `NexradSource` responsibilities against CloudFerro S3:
  ListObjectsV2 on `<date>/<CC>/<site>/PVOL/`, pick newest timestamp,
  GET the DBZH object (+ VRADH object at the closest timestamp when
  the Velocity product is selected; TH ignored v1).
- Reuse `app/http_client` (UA/timeout/redirect) and `app/s3_listing`
  (the ListObjectsV2 parser from the dedup task) unchanged.
- Decode off the UI thread via the shared decode executor, publish
  through the same `volumeReady` path so timeline, tilts, derived
  products (Echo Tops/VIL need ≥2 tilts — DBZH objects carry 12),
  probing, and GIF export work unmodified.
- Refresh cadence: follow the overlay timer (60 s default); volumes
  appear every 5 min per site.

### Catalog + UI

- Vendor `OPERA_RADARS.csv` → generated `europeSites()` list (script
  run once, output committed; ~220 sites with id, name, lat/lon,
  ODIM code, country segment). Station picker, favorites, chase, and
  the Esri basemap all work unchanged (panels are projection-agnostic).
- CC BY 4.0 credit: extend the on-canvas attribution chip when the
  active panel shows OPERA data + a line in README/docs.
- Settings: nothing new except the HDF5 requirement note. No API key
  fields needed (anonymous S3).

### Verification

- `tests/live_europe_smoke.ps1`: anonymous S3 list of one site's PVOL
  prefix, HEAD of newest DBZH object (magic HDF5 `\x89HDF`), decode
  smoke of first gates.
- UI tests: ODIM fixture through `OdimReader` → `DecodedChunk` →
  canvas probe asserts (mirrors the synthetic Message 31 path).
- No `CAPPEDWEATHER_*` credentials in CI: the live smoke must pass
  anonymously or not at all.

### Risks

- Anonymous S3 throttling (we hit 429 only on the API, never on S3;
  CloudFerro may still rate-limit heavy polling — bounded-parallel
  downloads + 60 s cadence already respect this).
- VRADH/DBZH schedule skew: different elevation tables per moment;
  associate by closest timestamp within ±5 min or show DBZH-only.
- Elevation tables vary per site/scan (see the nohur filename pair);
  never hardcode tilt lists — always parse from the object key +
  file.
- 24 h rolling window: document clearly that European timeline depth
  is one day, not eight volumes of deep history.
