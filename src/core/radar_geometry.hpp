#pragma once

#include <cstddef>
#include <optional>

namespace cappedweather {

struct GeographicCoordinate {
    double latitude = 0.0;
    double longitude = 0.0;
};

struct RangeBearing {
    double distanceMeters = 0.0;
    double bearingDegrees = 0.0;
};

struct BeamGeometry {
    double surfaceDistanceMeters = 0.0;
    double heightAboveRadarMeters = 0.0;
};

GeographicCoordinate destinationPoint(GeographicCoordinate origin, double bearingDegrees,
                                      double surfaceDistanceMeters);
RangeBearing rangeAndBearing(GeographicCoordinate origin, GeographicCoordinate destination);
BeamGeometry standardBeamGeometry(double slantRangeMeters, double elevationDegrees);
double slantRangeForSurfaceDistance(double surfaceDistanceMeters, double elevationDegrees);
double gateCenterSlantRange(int firstGateMeters, int gateSpacingMeters, std::size_t gate);
std::optional<std::size_t> gateIndexForSlantRange(double slantRangeMeters,
                                                  int firstGateMeters,
                                                  int gateSpacingMeters,
                                                  std::size_t gateCount);

} // namespace cappedweather
