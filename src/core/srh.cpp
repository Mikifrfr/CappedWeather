#include "core/srh.hpp"

#include <algorithm>
#include <cmath>

namespace cappedweather {

namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

struct WindPoint {
    double agl = 0.0;
    double u = 0.0;
    double v = 0.0;
};

}  // namespace

SrhEstimate estimateSrh(const std::vector<VadLevel>& levels, const BunkersMotion& stormMotion,
                        const double radarElevationMeters, const SrhConfig config) {
    SrhEstimate estimate;
    if (!stormMotion.valid) return estimate;
    // Resolve storm motion to (cx, cy): right-mover when the shear splits,
    // mean wind when weak. Same choice the scan dialog prints.
    const double useFrom =
        stormMotion.weakShear ? stormMotion.meanFromDegrees : stormMotion.rightFromDegrees;
    const double useSpeed =
        stormMotion.weakShear ? stormMotion.meanSpeedMps : stormMotion.rightSpeedMps;
    if (!std::isfinite(useFrom) || !std::isfinite(useSpeed) || useSpeed < 0.0) return estimate;
    const double toRad = (useFrom + 180.0) * kDegToRad;
    const double cx = useSpeed * std::sin(toRad);
    const double cy = useSpeed * std::cos(toRad);
    if (!std::isfinite(cx) || !std::isfinite(cy)) return estimate;

    std::vector<WindPoint> points;
    for (const auto& level : levels) {
        if (level.quality != VadQuality::Good && level.quality != VadQuality::Marginal)
            continue;
        if (!std::isfinite(level.heightMslMeters) || !std::isfinite(level.directionFromDegrees) ||
            !std::isfinite(level.speedMetersPerSecond))
            continue;
        const double agl = level.heightMslMeters - radarElevationMeters;
        if (!std::isfinite(agl) || agl < 0.0 || agl > config.layer03TopAglMeters) continue;
        const double rowTo = (level.directionFromDegrees + 180.0) * kDegToRad;
        const double u = level.speedMetersPerSecond * std::sin(rowTo);
        const double v = level.speedMetersPerSecond * std::cos(rowTo);
        if (!std::isfinite(u) || !std::isfinite(v)) continue;
        points.push_back({agl, u, v});
    }
    std::sort(points.begin(), points.end(),
              [](const WindPoint& a, const WindPoint& b) { return a.agl < b.agl; });
    const auto layerSrh = [&](const double topAgl, const double minSpan, const int minPairs,
                          bool& layerValid) {
        double srh = 0.0;
        layerValid = false;
        int pairs = 0;
        double bottom = 0.0, top = 0.0;
        bool first = true;
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            if (points[i].agl > topAgl || points[i + 1].agl > topAgl) break;
            srh += (points[i + 1].u - cx) * (points[i].v - cy) -
                   (points[i].u - cx) * (points[i + 1].v - cy);
            if (first) {
                bottom = points[i].agl;
                first = false;
            }
            top = points[i + 1].agl;
            ++pairs;
        }
        if (pairs >= minPairs && top - bottom >= minSpan && std::isfinite(srh)) {
            layerValid = true;
            return srh;
        }
        return 0.0;
    };
    estimate.srh01M2PerS2 =
        layerSrh(config.layer01TopAglMeters, config.minSpanMeters, 1, estimate.srh01Valid);
    estimate.srh03M2PerS2 =
        layerSrh(config.layer03TopAglMeters, config.minSpan03Meters, config.minPairs03,
                 estimate.srh03Valid);
    if (!estimate.srh01Valid && !estimate.srh03Valid) return estimate;
    estimate.valid = true;
    estimate.quality = (estimate.srh01Valid && estimate.srh03Valid && points.size() >= 4)
                           ? VadQuality::Good
                           : VadQuality::Marginal;
    return estimate;
}

}  // namespace cappedweather
