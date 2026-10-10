#pragma once

// GOES-R ABI L1b CONUS (RadC) infrared: one Rad(y,x) field plus the fixed
// grid record needed to place cells. Radiance converts to brightness
// temperature at load via the file's Planck coefficients (fk1/fk2/bc1/bc2),
// so the canvas paints a meteorological quantity, not counts. Missing
// gates stay NaN. Pure core types, no Qt, deterministic.

#include "core/fixedgrid.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace cappedweather {

struct GoesFrame {
    int channel = 0;
    long long validEpochSeconds = 0;  // scan start (t variable)
    std::size_t nx = 0;
    std::size_t ny = 0;
    double x0Radians = 0.0;  // x[0]: western edge as stored
    double y0Radians = 0.0;  // y[0]: first row as stored
    double dxRadians = 0.0;
    double dyRadians = 0.0;
    GoesFixedGrid grid;
    // Row-major brightness temperature in Celsius, index iy * nx + ix.
    // NaN = missing/off-disc.
    std::vector<float> brightnessCelsius;
};

// True when the core was built with netCDF support (CAPPEDWEATHER_HAS_NETCDF).
bool goesReaderAvailable();

// Reads one ABI L1b RadC file (NetCDF-4). Nullopt when netCDF support is
// unavailable or the file is malformed.
std::optional<GoesFrame> readGoesRad(const std::string& path);

}  // namespace cappedweather
