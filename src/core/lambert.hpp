#pragma once

// Lambert Conformal Conic, spherical earth (Snyder): the CF grid_mapping
// used by HRRR (single tangent parallel) and most NWS services. Forward
// projects lon/lat degrees to meters, inverse recovers lon/lat. One or two
// standard parallels; central meridian in degrees east. Pure, no UI,
// deterministic. NaN in, NaN out.

#include <cmath>
#include <limits>

namespace cappedweather {

struct LambertConformal {
    double latitudeOriginDeg = 0.0;
    double longitudeMeridianEastDeg = 0.0;
    double firstParallelDeg = 0.0;
    double secondParallelDeg = 0.0;  // equal to first for tangent cones
    double earthRadiusMeters = 6371229.0;
};

struct LambertXy {
    double xMeters = std::numeric_limits<double>::quiet_NaN();
    double yMeters = std::numeric_limits<double>::quiet_NaN();
};

struct LambertLonLat {
    double longitudeDeg = std::numeric_limits<double>::quiet_NaN();
    double latitudeDeg = std::numeric_limits<double>::quiet_NaN();
};

LambertXy lambertForward(const LambertConformal& projection, double longitudeDeg,
                         double latitudeDeg);
LambertLonLat lambertInverse(const LambertConformal& projection, double xMeters, double yMeters);

}  // namespace cappedweather
