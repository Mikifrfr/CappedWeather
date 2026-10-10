#include "core/sounding.hpp"

#include <algorithm>

namespace cappedweather {

namespace {

constexpr double kKnotsToMps = 0.5144444444444445;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

}  // namespace

std::vector<SrhWindPoint> soundingWindPoints(const Sounding& sounding) {
    std::vector<SrhWindPoint> points;
    double surfaceMsl = std::numeric_limits<double>::quiet_NaN();
    for (const auto& level : sounding.levels) {
        if (std::isfinite(level.heightMslMeters)) {
            if (!std::isfinite(surfaceMsl)) surfaceMsl = level.heightMslMeters;
            break;
        }
    }
    if (!std::isfinite(surfaceMsl)) return points;
    for (const auto& level : sounding.levels) {
        if (!std::isfinite(level.heightMslMeters) || !std::isfinite(level.directionFromDegrees) ||
            !std::isfinite(level.speedKnots) || level.speedKnots < 0.0)
            continue;
        const double agl = level.heightMslMeters - surfaceMsl;
        if (!std::isfinite(agl) || agl < 0.0) continue;
        const double toRad = (level.directionFromDegrees + 180.0) * kDegToRad;
        const double speedMps = level.speedKnots * kKnotsToMps;
        const double u = speedMps * std::sin(toRad);
        const double v = speedMps * std::cos(toRad);
        if (!std::isfinite(u) || !std::isfinite(v)) continue;
        points.push_back({agl, u, v});
    }
    return points;
}

double soundingFreezingLevelMsl(const Sounding& sounding) {
    for (std::size_t i = 1; i < sounding.levels.size(); ++i) {
        const auto& below = sounding.levels[i - 1];
        const auto& above = sounding.levels[i];
        if (!std::isfinite(below.tempCelsius) || !std::isfinite(above.tempCelsius) ||
            !std::isfinite(below.heightMslMeters) || !std::isfinite(above.heightMslMeters))
            continue;
        if (below.tempCelsius >= 0.0 && above.tempCelsius < 0.0 &&
            above.heightMslMeters > below.heightMslMeters) {
            const double fraction =
                below.tempCelsius / (below.tempCelsius - above.tempCelsius);
            return below.heightMslMeters +
                   fraction * (above.heightMslMeters - below.heightMslMeters);
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

}  // namespace cappedweather
