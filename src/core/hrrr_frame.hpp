#pragma once

// HRRR simulated composite reflectivity over a TDS NCSS subset: one 2D
// REFC field on its native Lambert grid plus the projection record needed
// to place cells on the map. The canvas inverts each visible cell to
// lat/lon at paint time (viewport-culled; row orientation never assumed).
// Valid time decodes from the time variable's units ("Hour since ...").
// Missing gates stay NaN via _FillValue/missing_value when present, else
// any non-finite raw value. Pure core types, no Qt, deterministic.

#include "core/lambert.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace cappedweather {

struct HrrrFrame {
    long long validEpochSeconds = 0;
    std::size_t nx = 0;
    std::size_t ny = 0;
    double x0Km = 0.0;  // x[0]: westernmost column
    double y0Km = 0.0;  // y[0]: first row as stored (orientation discovered at runtime)
    double dxKm = 0.0;
    double dyKm = 0.0;
    LambertConformal projection;
    // Row-major reflectivity in dB, index iy * nx + ix. NaN = missing.
    std::vector<float> reflectivityDbz;
};

// True when the core was built with netCDF support (CAPPEDWEATHER_HAS_NETCDF).
bool hrrrReaderAvailable();

// Reads one TDS NCSS REFC subset file (NetCDF-3 or 4). First time step
// only. Nullopt when netCDF support is unavailable or the file is malformed.
std::optional<HrrrFrame> readHrrrRefc(const std::string& path);

}  // namespace cappedweather
