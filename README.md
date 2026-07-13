# CappedWeather

CappedWeather is a small Qt6/C++ NEXRAD viewer focused on direct operational data.

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

The application uses public HTTPS listing and download requests, with no cloud SDK. The official NWS station catalog is filtered to `WSR-88D` stations; the embedded fallback is the official WSR-88D list and contains no generated stations.

The viewer exposes the decoded product fields and the elevation tilts present in the received volume. It does not render an echo until a real Level II chunk has been downloaded and decoded.

## Current viewer features

- Six native Level II products with product-specific color scales and units.
- Cursor sampling for value, range, azimuth, and elevation tilt, including below-threshold and range-folded gates.
- Up to eight recent complete NOAA volumes in memory, with frame stepping, a timeline slider, looping playback, and 1x/2x/4x speeds.
- Active NWS polygon warnings and current-day SPC tornado, hail, and wind reports, with independent visibility controls.
- Cached, color-batched radar geometry so map panning reuses decoded gate paths instead of rebuilding every gate.

Only alerts carrying explicit GeoJSON polygon geometry are drawn. NWS alerts whose area is represented solely by forecast-zone identifiers remain available from NWS but are not approximated by this viewer.
