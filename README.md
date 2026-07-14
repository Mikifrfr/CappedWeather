# CappedWeather

CappedWeather is a small Qt6/C++ WSR-88D and TDWR Level II viewer focused on direct operational data.

The application does not ship synthetic or demo radar frames. It discovers live Level II objects from NOAA's public AWS buckets and displays an explicit unavailable/stale state when data cannot be obtained.

## Build

The core library and tests require only a C++20 compiler and CMake:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The optional live-data smoke test uses only real public NOAA/NWS responses:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\live_data_smoke.ps1 -RadarId KTLX
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\live_data_smoke.ps1 -RadarId TATL
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\live_overlays_smoke.ps1
```

When Qt6 and BZip2 are available, CMake also builds the `CappedWeather` desktop target. Set `CMAKE_PREFIX_PATH` to the Qt installation if it is not discoverable:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64"
```

The GUI target additionally needs BZip2. On Windows, pass the include directory and static library explicitly if CMake cannot discover it:

```powershell
cmake -S . -B build `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64" `
  -DBZIP2_INCLUDE_DIR="C:/path/to/bzip2" `
  -DBZIP2_LIBRARY_RELEASE="C:/path/to/libbz2.lib"
```

## Data sources

- Real-time Level II chunks: `https://unidata-nexrad-level2-chunks.s3.amazonaws.com/`
- Level II archive: `https://unidata-nexrad-level2.s3.amazonaws.com/`
- Live official station catalog: `https://api.weather.gov/radar/stations`
- Official NEXRAD data documentation: <https://www.ncei.noaa.gov/products/radar/next-generation-weather-radar>

The application uses public HTTPS listing and download requests, with no cloud SDK. The official NWS station catalog is filtered to `WSR-88D` and `TDWR` stations; the embedded fallback contains the same real station classes and no generated stations.

The viewer exposes the decoded product fields and the elevation tilts present in the received volume. It does not render an echo until a real Level II chunk has been downloaded and decoded.

## Current viewer features

- Six native Level II products with product-specific color scales and units.
- TDWR volumes expose their available base moments (reflectivity, radial velocity, and spectrum width); unavailable dual-polarization products are disabled automatically.
- Searchable radar selection with persistent favorites, recent sites, zoom-aware station decluttering, station details, and national/recenter controls.
- Cursor sampling for value, range, azimuth, and elevation tilt, including below-threshold and range-folded gates.
- Up to eight recent NOAA volumes in memory, with explicit live-follow mode, timestamped frame stepping, looping playback, 1x/2x/4x speeds, and repeated supplemental tilts retained separately.
- Non-blocking chunk progress, retry/cancel controls, per-chunk automatic retries, partial-volume recovery, and visible completeness metadata.
- Active NWS polygon warnings and current-day SPC tornado, hail, and wind reports, with filters, a symbol legend, and interactive details.
- Multiple range rings, distance/bearing measurement, keyboard controls, radar opacity, below-threshold display, and three palette presets.
- Cached, color-batched radar geometry so map panning reuses decoded gate paths instead of rebuilding every gate.

Only alerts carrying explicit GeoJSON polygon geometry are drawn. NWS alerts whose area is represented solely by forecast-zone identifiers remain available from NWS but are not approximated by this viewer.
