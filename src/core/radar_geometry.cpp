#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace cappedweather {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusMeters = 6371000.0;
constexpr double kEffectiveEarthRadiusMeters = kEarthRadiusMeters * 4.0 / 3.0;

double radians(const double degrees) {
    return degrees * kPi / 180.0;
}

double degrees(const double radiansValue) {
    return radiansValue * 180.0 / kPi;
}
}

GeographicCoordinate destinationPoint(const GeographicCoordinate origin, const double bearingDegrees,
                                      const double surfaceDistanceMeters) {
    const auto angularDistance = std::max(0.0, surfaceDistanceMeters) / kEarthRadiusMeters;
    const auto bearing = radians(bearingDegrees);
    const auto latitude = radians(origin.latitude);
    const auto longitude = radians(origin.longitude);
    const auto destinationLatitude = std::asin(
        std::sin(latitude) * std::cos(angularDistance) +
        std::cos(latitude) * std::sin(angularDistance) * std::cos(bearing));
    const auto destinationLongitude = longitude + std::atan2(
        std::sin(bearing) * std::sin(angularDistance) * std::cos(latitude),
        std::cos(angularDistance) - std::sin(latitude) * std::sin(destinationLatitude));
    auto normalizedLongitude = std::fmod(degrees(destinationLongitude) + 540.0, 360.0) - 180.0;
    return {degrees(destinationLatitude), normalizedLongitude};
}

RangeBearing rangeAndBearing(const GeographicCoordinate origin, const GeographicCoordinate destination) {
    const auto latitude1 = radians(origin.latitude);
    const auto latitude2 = radians(destination.latitude);
    const auto deltaLatitude = latitude2 - latitude1;
    const auto deltaLongitude = radians(destination.longitude - origin.longitude);
    const auto a = std::sin(deltaLatitude / 2.0) * std::sin(deltaLatitude / 2.0) +
                   std::cos(latitude1) * std::cos(latitude2) *
                       std::sin(deltaLongitude / 2.0) * std::sin(deltaLongitude / 2.0);
    const auto distance = 2.0 * kEarthRadiusMeters *
                          std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
    const auto y = std::sin(deltaLongitude) * std::cos(latitude2);
    const auto x = std::cos(latitude1) * std::sin(latitude2) -
                   std::sin(latitude1) * std::cos(latitude2) * std::cos(deltaLongitude);
    auto bearing = std::fmod(degrees(std::atan2(y, x)) + 360.0, 360.0);
    return {distance, bearing};
}

BeamGeometry standardBeamGeometry(const double slantRangeMeters, const double elevationDegrees) {
    const auto range = std::max(0.0, slantRangeMeters);
    const auto elevation = radians(elevationDegrees);
    const auto height = std::sqrt(range * range + kEffectiveEarthRadiusMeters * kEffectiveEarthRadiusMeters +
                                  2.0 * range * kEffectiveEarthRadiusMeters * std::sin(elevation)) -
                        kEffectiveEarthRadiusMeters;
    const auto ratio = std::clamp(range * std::cos(elevation) /
                                      (kEffectiveEarthRadiusMeters + height),
                                  -1.0, 1.0);
    return {kEffectiveEarthRadiusMeters * std::asin(ratio), height};
}

double slantRangeForSurfaceDistance(const double surfaceDistanceMeters, const double elevationDegrees) {
    const auto target = std::max(0.0, surfaceDistanceMeters);
    auto lower = 0.0;
    auto upper = std::max(1000.0, target * 1.25 + 10000.0);
    while (standardBeamGeometry(upper, elevationDegrees).surfaceDistanceMeters < target) upper *= 2.0;
    for (int iteration = 0; iteration < 36; ++iteration) {
        const auto middle = (lower + upper) * 0.5;
        if (standardBeamGeometry(middle, elevationDegrees).surfaceDistanceMeters < target) lower = middle;
        else upper = middle;
    }
    return (lower + upper) * 0.5;
}

} // namespace cappedweather
