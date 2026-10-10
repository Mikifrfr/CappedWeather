#include "core/fixedgrid.hpp"

#include <algorithm>

namespace cappedweather {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr double kRadToDeg = 180.0 / kPi;

}  // namespace

GoesLonLat goesFixedInverse(const GoesFixedGrid& grid, const double xRadians,
                            const double yRadians) {
    if (!std::isfinite(xRadians) || !std::isfinite(yRadians) ||
        !(grid.perspectiveHeightMeters > 0.0) || !(grid.semiMajorMeters > 0.0) ||
        !(grid.semiMinorMeters > 0.0))
        return {};
    const double req = grid.semiMajorMeters;
    const double rpol = grid.semiMinorMeters;
    const double bigH = grid.perspectiveHeightMeters + req;
    // Ellipsoid intersection along the sight line (earth-centered frame,
    // x through the subpoint, y west-positive by the sign convention of
    // the lon formula below). a carries the ellipsoid flattening, so
    // near-limb ranges land on the disc instead of inside it: with a
    // spherical a the file corners missed their recorded bounds by 20 deg.
    const double cosX = std::cos(xRadians);
    const double cosY = std::cos(yRadians);
    const double sinX = std::sin(xRadians);
    const double sinY = std::sin(yRadians);
    const double flattenRatio = (req * req) / (rpol * rpol);
    const double a = cosY * cosY + flattenRatio * sinY * sinY;
    const double b = -2.0 * bigH * cosX * cosY;
    const double c = bigH * bigH - req * req;
    const double discriminant = b * b - 4.0 * a * c;
    if (!std::isfinite(a) || a == 0.0 || !std::isfinite(discriminant) || discriminant < 0.0)
        return {};
    // Near intersection (minus root): the plus root is the far side of the
    // ellipsoid, behind the satellite (sx > H).
    const double rs = -(b + std::sqrt(discriminant)) / (2.0 * a);
    if (!std::isfinite(rs) || rs <= 0.0) return {};
    const double sx = rs * cosX * cosY;
    const double sy = -rs * sinX;
    const double sz = rs * cosX * sinY;
    const double ratio = flattenRatio;
    const double lat = std::atan(ratio * sz / std::hypot(bigH - sx, sy));
    const double lon = grid.subpointLongitudeDeg * kDegToRad - std::atan2(sy, bigH - sx);
    GoesLonLat out;
    out.latitudeDeg = lat * kRadToDeg;
    // Conventional -180..180 longitudes out.
    double lonDeg = lon * kRadToDeg;
    lonDeg = std::fmod(lonDeg + 540.0, 360.0) - 180.0;
    out.longitudeDeg = lonDeg;
    if (!std::isfinite(out.latitudeDeg) || !std::isfinite(out.longitudeDeg)) return {};
    return out;
}

GoesAngles goesFixedForward(const GoesFixedGrid& grid, const double longitudeDeg,
                            const double latitudeDeg) {
    if (!std::isfinite(longitudeDeg) || !std::isfinite(latitudeDeg) ||
        !(grid.perspectiveHeightMeters > 0.0) || !(grid.semiMajorMeters > 0.0) ||
        !(grid.semiMinorMeters > 0.0))
        return {};
    const double req = grid.semiMajorMeters;
    const double rpol = grid.semiMinorMeters;
    const double bigH = grid.perspectiveHeightMeters + req;
    const double ecc2 = 1.0 - (rpol * rpol) / (req * req);
    const double lat = latitudeDeg * kDegToRad;
    double dlon = (longitudeDeg - grid.subpointLongitudeDeg) * kDegToRad;
    dlon = std::fmod(dlon + 3.0 * kPi, 2.0 * kPi) - kPi;
    const double geocentric = std::atan((rpol * rpol) / (req * req) * std::tan(lat));
    const double rc =
        rpol / std::sqrt(std::max(1e-12, 1.0 - ecc2 * std::cos(geocentric) * std::cos(geocentric)));
    if (!std::isfinite(rc) || rc <= 0.0) return {};
    // Earth-centered ground point, y east-positive. The angle pair is the
    // exact inverse of goesFixedInverse above: x from the full look range,
    // y from the nadir-ward leg (the hypot form drifts degrees off away
    // from the subpoint meridian).
    const double gx = rc * std::cos(geocentric) * std::cos(dlon);
    const double gy = rc * std::cos(geocentric) * std::sin(dlon);
    const double gz = rc * std::sin(geocentric);
    const double qx = gx - bigH;
    const double range = std::hypot(qx, std::hypot(gy, gz));
    if (!std::isfinite(range) || range == 0.0) return {};
    GoesAngles out;
    out.xRadians = std::asin(std::clamp(gy / range, -1.0, 1.0));
    out.yRadians = std::atan2(gz, bigH - gx);
    if (!std::isfinite(out.xRadians) || !std::isfinite(out.yRadians)) return {};
    return out;
}

}  // namespace cappedweather
