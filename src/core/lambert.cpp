#include "core/lambert.hpp"

namespace cappedweather {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

double coneConstant(const double lat1, const double lat2) {
    if (std::abs(lat1 - lat2) < 1e-10) return std::sin(lat1);
    return std::log(std::cos(lat1) / std::cos(lat2)) /
           std::log(std::tan(kPi / 4.0 + lat2 / 2.0) / std::tan(kPi / 4.0 + lat1 / 2.0));
}

double tOfLat(const double lat) {
    return std::tan(kPi / 4.0 + lat / 2.0);
}

// Smallest signed difference a-b in degrees (-180, 180]: -95 and 265 E
// are the same meridian and must difference to zero, not -360.
double wrapDeltaLon(const double a, const double b) {
    double delta = std::fmod(a - b, 360.0);
    if (delta <= -180.0) delta += 360.0;
    else if (delta > 180.0) delta -= 360.0;
    return delta;
}

}  // namespace

LambertXy lambertForward(const LambertConformal& projection, const double longitudeDeg,
                         const double latitudeDeg) {
    if (!std::isfinite(longitudeDeg) || !std::isfinite(latitudeDeg) ||
        !std::isfinite(projection.earthRadiusMeters) || projection.earthRadiusMeters <= 0.0)
        return {};
    const double lat1 = projection.firstParallelDeg * kDegToRad;
    const double lat2 = projection.secondParallelDeg * kDegToRad;
    const double lat0 = projection.latitudeOriginDeg * kDegToRad;
    const double lat = latitudeDeg * kDegToRad;
    const double n = coneConstant(lat1, lat2);
    if (!std::isfinite(n) || std::abs(n) < 1e-12) return {};
    const double bigF = std::cos(lat1) * std::pow(tOfLat(lat1), n) / n;
    const double rho = projection.earthRadiusMeters * bigF / std::pow(tOfLat(lat), n);
    const double rho0 = projection.earthRadiusMeters * bigF / std::pow(tOfLat(lat0), n);
    if (!std::isfinite(rho) || !std::isfinite(rho0)) return {};
    const double theta = n * wrapDeltaLon(longitudeDeg, projection.longitudeMeridianEastDeg) *
                         kDegToRad;
    return {rho * std::sin(theta), rho0 - rho * std::cos(theta)};
}

LambertLonLat lambertInverse(const LambertConformal& projection, const double xMeters,
                             const double yMeters) {
    if (!std::isfinite(xMeters) || !std::isfinite(yMeters) ||
        !std::isfinite(projection.earthRadiusMeters) || projection.earthRadiusMeters <= 0.0)
        return {};
    const double lat1 = projection.firstParallelDeg * kDegToRad;
    const double lat2 = projection.secondParallelDeg * kDegToRad;
    const double lat0 = projection.latitudeOriginDeg * kDegToRad;
    const double lon0 = projection.longitudeMeridianEastDeg * kDegToRad;
    const double n = coneConstant(lat1, lat2);
    if (!std::isfinite(n) || std::abs(n) < 1e-12) return {};
    const double bigF = std::cos(lat1) * std::pow(tOfLat(lat1), n) / n;
    const double rho0 = projection.earthRadiusMeters * bigF / std::pow(tOfLat(lat0), n);
    const double rho = (n < 0.0 ? -1.0 : 1.0) *
                       std::hypot(xMeters, rho0 - yMeters);
    if (!std::isfinite(rho) || rho == 0.0) return {};
    const double theta = std::atan2(xMeters, rho0 - yMeters);
    const double t = std::pow(projection.earthRadiusMeters * bigF / rho, 1.0 / n);
    if (!std::isfinite(t) || t <= 0.0) return {};
    // Inverse of the forward branch above (t = tan(pi/4 + lat/2)):
    // lat = 2*atan(t) - pi/2, not the pi/2 - 2*atan(t) of the mirrored form.
    const double lat = 2.0 * std::atan(t) - kPi / 2.0;
    const double lon = lon0 + theta / n;
    LambertLonLat out;
    // Report conventional -180..180 longitudes regardless of how the
    // central meridian was written (265 E in, -95 out).
    out.longitudeDeg = wrapDeltaLon(lon * kRadToDeg, 0.0);
    out.latitudeDeg = lat * kRadToDeg;
    if (!std::isfinite(out.longitudeDeg) || !std::isfinite(out.latitudeDeg)) return {};
    return out;
}

}  // namespace cappedweather
