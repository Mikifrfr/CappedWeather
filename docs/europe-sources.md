# Europe: OPERA/ODIM volumes: verified ground truth + plan

All endpoints below were probed live on 2026-09-05. No API keys were used.
User direction: verify access first, skip the composite-layer detour, go
straight at ODIM polar volumes.

## Radar-mosaic basemap presets (verified live 2026-10-10)

Settings: Basemap keeps a Custom XYZ slot with one-click presets, all
keyless and verified with HTTP 200 PNG tiles:

- IEM US NEXRAD mosaic: fixed template
  `https://mesonet.agron.iastate.edu/cache/tile.py/1.0.0/nexrad-n0q-900913/{z}/{x}/{y}.png`,
  credit Iowa Environmental Mesonet. CONUS reflectivity composite, 5-minute
  refresh on IEM's side.
- RainViewer global mosaic (covers Canadian ECCC radars plus 1200+ radars
  in 150+ countries): anonymous metadata at
  `https://api.rainviewer.com/public/weather-maps.json` pins the newest
  past frame to a static template
  `{host}{path}/256/{z}/{x}/{y}/2/1_1.png` (frame paths rotate every
  5 minutes and are hash-based; timestamps went 410 Gone in 2026, so the
  path always comes from the payload). The preset button fetches and fills
  it; `rainviewerTileTemplate()` is hermetic-tested on a live-shaped
  payload and fails closed on bad JSON or empty frame lists.

Both flow through the production tile URL builder, so offline packs and
the painted attribution pick them up unchanged. They render radar on a
dark background: context for chasing, not a replacement for Level II
volumes.

## What is open (verified)

### 1. OPERA single-site volumes on anonymous S3: the primary path

- Bucket `openradar-24h` on CloudFerro S3, anonymous List + Get:
  `https://s3.waw3-1.cloudferro.com/openradar-24h?list-type=2&prefix=...`
- Layout: `YYYY/MM/DD/CC/site/<PVOL|SCAN>/`, e.g. `2026/09/05/NO/nohur/PVOL/`.
  Providers disagree on container: PVOL sites (Norway, Netherlands) upload one
  object per moment covering N elevations; SCAN sites (France, Germany,
  Finland, Sweden, ...) upload one object per elevation. The app lists both
  prefixes and prefers PVOL when present, otherwise assembles a volume from
  the SCAN elevations sharing a 5-minute scan bucket.
- Object naming: `<site>@<YYYYMMDDTHHMM>@<elevations>@<MOMENTS>.h5`
  e.g. `nohur@20260905T0000@0.5_1.0_..._15.5@DBZH.h5`
  (~0.5–2 MB for separate moments, up to ~24 MB for combined). The filename
  itself lists every elevation for PVOL objects: tilt discovery without
  downloading. Moment lists vary: separate `@DBZH.h5` / `@TH.h5` / `@VRADH.h5`
  vs combined `@DBZH_TH_VRADH.h5` (FI/SE/FR/DE SCAN) vs
  `@CCORH_DBZH_PHIDP_RHOHV_SQIH_TH_VRADH_WRADH_ZDR.h5` (NL PVOL). Any object
  whose moment list contains DBZH is treated as reflectivity; TH backs
  Reflectivity only when an elevation carries no DBZH.
- One PVOL object = one moment × N elevations. A PVOL "volume" is a single
  GET (compare: NEXRAD needs ~70 chunk GETs). A SCAN "volume" is one GET per
  elevation in the scan bucket (typically 5–14 small files). Moments ride
  separate objects on their own schedules on PVOL sites (DBZH/TH share a scan
  table, VRADH interleaves its own, e.g. `...T0006@2.6_..._90.0@VRADH.h5`).
  SCAN sites sweep elevations across minutes (FR staggers a volume over
  ~5 min), so assembly buckets by 5-minute scan cycle, not exact timestamp.
- Moments available now: DBZH (QC reflectivity), TH (unfiltered
  reflectivity), VRADH (radial velocity). Dual-pol (CC, ZDR, PHI, KDP)
  is announced for 2027+. This maps exactly onto the existing TDWR
  pattern: three base moments, dual-pol auto-disabled.
- Velocity folds at the scan's own Nyquist (`how/NI` per dataset, e.g.
  HRGOL 6.1/8.3/16.0 m/s by tilt), not the panel default. Rays carry
  their dataset NI into `DecodedRay` and unfolding uses it; unfolding
  C-band single-PRF data at a 25 m/s default left fold boundaries in
  the grid that passed meso gates at close range (phantom HRGOL
  cluster, fixed 2026-10-08, zero couplets on the same file after).
- Rolling 24 h cache only. There is no European equivalent of the AWS
  Level II archive on this bucket (long-term archive "2012–, TBD" is
  API-only). Timeline depth for Europe is limited to the last day.
- Format: ODIM_H5 polar volumes (HDF5). Requires an HDF5 reader
  (vcpkg `hdf5`, same REQUIRE-gate pattern as netCDF).

### 2. ORD API (MeteoGate): catalog + metadata, keyed for quota

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
  (219 radar rows with lat/lon, ODIM code, WIGOS id, band, max range).
  The S3 country segment is the uppercased first two letters of the
  ODIM code (`nohur` → `NO`, `deess` → `DE`, `bezav` → `BE`).
  Upstream "operational" status does NOT track bucket sharing: as of
  2026-09-06 ten national networks share nothing (ES/UK/HU/SK/GR/PT/RS/IL/LV/MD)
  and are excluded from the app catalog, while eight sharing sites the CSV
  flags non-operational are included (DK/FI/IS/PL/LT additions) plus the
  HRZAG→HRPUN rename. `tools/generate_opera_sites.py` holds these lists;
  `tools/check_opera_availability.py` re-probes the bucket (132 sites).

### 3. DWD open data: fallback / Germany depth (probed live, no key)

- `https://opendata.dwd.de/weather/radar/composite/` → `wn/ pg/ rv/
  vii/ rs/ hg/ hx/ dmax/ hymecng/`, 5-minute `WN<date>.tar.bz2` files.
- `.../radolan/`, `.../sites/`, `.../mesocyclones/`, `.../konrad3d/`
  also live. Formats are DWD-specific (BUFR, RADOLAN binary,
  run-length products): NOT ODIM. Only worth it if OPERA misses a
  German site or for mesocyclone markers later.

### 4. Licensing

- OPERA data: CC BY 4.0. The app must credit EUMETNET OPERA wherever
  European data renders (map chip + docs + release notes). Same class
  of requirement as the CARTO/OSM attribution already painted on-canvas.

### Explicitly out

- OPERA NIMBUS composites as a map layer (user: skip the detour).
- KNMI/SMHI/FMI national APIs (per-service keys, redundant once OPERA
  volumes flow; revisit only for sites OPERA lacks).
- Meteoalarm warnings: live with full map geometry, per-country hazard
  feeds from the visualization service (`visservice.meteoalarm.org`, same
  backend as meteoalarm.org) joined to the official warning-area polygons
  from its vector tiles by area UUID, with per-area warning texts
  (`WeatherOverlaySource` + map polygons + `AlertCenter` + severity/age/
  proximity filters). Awareness levels 1/2/3 map to Extreme/Severe/Moderate
  in official red/orange/yellow. Details and tile geometry cache per refresh
  round with bounded per-round requests (most-severe-first).
- OPERA BUFR volumes: HDF5 covers everything recent; do not write a
  BUFR decoder.

## Implementation plan: ODIM volumes as first-class radar sessions

Goal: a German/Dutch/Norwegian site behaves exactly like a US one:
pick station, LIVE follow, tilts, products, timeline, cross-sections.

### Core: `src/core/odim_reader.{hpp,cpp}` + tests

- Parse ODIM_H5 PVOL with HDF5-C: `/datasetN/{dataM/data,what/quantity}`,
  `/datasetN/where/elangle,nbins,nrays,rstart,rscale`,
  `data{what/gain,offset,nodata,undetect}`.
- Map moments: DBZH → Reflectivity, VRADH → Velocity, TH → (hold;
  expose as Reflectivity only if DBZH absent, else ignore, decide in
  review; never invent a ninth product silently).
- Emit the existing `DecodedRay` model: one ray per azimuth per
  elevation, gate values decoded with gain/offset, nodata/undetect
  mapped to the existing below-threshold/missing sentinels.
- Elevation numbers: assign sequentially sorted by elangle (ODIM has
  no stable tilt numbering across sites).
- Unit tests: hand-built minimal ODIM_H5? netCDF-style stub problem:
  HDF5 has no hand-rolled writer. Options: (a) commit a tiny real
  fixture file (~100 KB, one elevation, CC BY 4.0, attributed);
  (b) generate the fixture at test time with the HDF5 lib when
  available, skip otherwise. Prefer (a): deterministic, tests the
  reader exactly as shipped.
- Build gating: `CAPPEDWEATHER_REQUIRE_HDF5` default ON for production
  (same fail-fast philosophy as netCDF), OFF for stub CI.

### App: `src/app/opera_source.{hpp,cpp}`

- Mirrors `NexradSource` responsibilities against CloudFerro S3:
  ListObjectsV2 on `<date>/<CC>/<site>/{PVOL,SCAN}/` (today + yesterday),
  pick newest timestamps, GET the DBZH objects (+ VRADH object at the closest
  timestamp when the Velocity product is selected on PVOL sites; TH ignored
  v1 unless no DBZH exists).
- PVOL volumes decode from a single object; SCAN volumes merge one object per
  elevation in the newest 5-minute scan bucket (elevations renumbered sorted
  by angle so tilts, timeline, Echo Tops/VIL, probing, and export work
  unmodified: SCAN volumes with ≥2 tilts derive ET/VIL like PVOL ones).
- Reuse `app/http_client` (UA/timeout/redirect) and `app/s3_listing`
  (the ListObjectsV2 parser from the dedup task) unchanged.
- Decode off the UI thread via the shared decode executor, publish
  through the same `volumeReady` path so timeline, tilts, derived
  products (Echo Tops/VIL need ≥2 tilts: PVOL DBZH objects carry 12,
  SCAN buckets typically 5–14), probing, and GIF export work unmodified.
- Refresh cadence: follow the overlay timer (60 s default); volumes
  appear every 5 min per site.

### Catalog + UI

- Vendor `OPERA_RADARS.csv` → generated `europeSites()` list (script
  run once, output committed; 132 bucket-active sites with id, name, lat/lon,
  ODIM code, country segment). Station picker, favorites, chase, and
  the basemap all work unchanged (panels are projection-agnostic).
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
  CloudFerro may still rate-limit heavy polling, bounded-parallel
  downloads + 60 s cadence already respect this).
- VRADH/DBZH schedule skew: different elevation tables per moment;
  associate by closest timestamp within ±5 min or show DBZH-only.
- Elevation tables vary per site/scan (see the nohur filename pair);
  never hardcode tilt lists. Always parse from the object key +
  file.
- 24 h rolling window: document clearly that European timeline depth
  is one day, not eight volumes of deep history.
