#pragma once

#include <cstddef>
#include <cstdint>
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

// Shared lat/lon grid-cell key: packs (x, y) into a single int64 so flash-rate
// grids, GLM overlays, and radar/lightning fusion agree on identity.
inline std::int64_t gridCellKey(const int cellX, const int cellY) {
    return (static_cast<std::int64_t>(cellX) << 32) |
           (static_cast<std::int64_t>(cellY) & 0xFFFFFFFFLL);
}

inline void unpackGridCellKey(const std::int64_t key, int& cellX, int& cellY) {
    cellX = static_cast<int>(key >> 32);
    cellY = static_cast<int>(key & 0xFFFFFFFFLL);
}

// Great-circle distance; thin wrapper over rangeAndBearing so UI filters share
// the core geodesic instead of duplicating haversine.
inline double haversineMeters(const double lat1, const double lon1, const double lat2,
                              const double lon2) {
    return rangeAndBearing(GeographicCoordinate{lat1, lon1},
                           GeographicCoordinate{lat2, lon2})
        .distanceMeters;
}

} // namespace cappedweather
