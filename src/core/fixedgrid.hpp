#pragma once

// GOES-R fixed grid (geostationary perspective, PUG 5.1.2.8): satellite
// view angles x/y in radians to lon/lat and back, WGS84 ellipsoid.
// Off-limb sight lines fail closed to NaN (negative discriminant), never
// clamped to the disc edge. Pure, no UI, deterministic. NaN in, NaN out.

#include <cmath>
#include <limits>

namespace cappedweather {

struct GoesFixedGrid {
    double perspectiveHeightMeters = 35786023.0;  // above the ellipsoid
    double semiMajorMeters = 6378137.0;
    double semiMinorMeters = 6356752.31414;
    double subpointLongitudeDeg = -137.0;
};

struct GoesLonLat {
    double longitudeDeg = std::numeric_limits<double>::quiet_NaN();
    double latitudeDeg = std::numeric_limits<double>::quiet_NaN();
};

struct GoesAngles {
    double xRadians = std::numeric_limits<double>::quiet_NaN();
    double yRadians = std::numeric_limits<double>::quiet_NaN();
};

GoesLonLat goesFixedInverse(const GoesFixedGrid& grid, double xRadians, double yRadians);
GoesAngles goesFixedForward(const GoesFixedGrid& grid, double longitudeDeg, double latitudeDeg);

}  // namespace cappedweather
