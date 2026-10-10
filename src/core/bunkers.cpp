#include "core/bunkers.hpp"

#include <cmath>

namespace cappedweather {

namespace {

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

// Meteorological FROM degrees + speed -> (u east+, v north+) m/s.
bool fromToUV(const double fromDegrees, const double speedMps, double& u, double& v) {
    if (!std::isfinite(fromDegrees) || !std::isfinite(speedMps) || speedMps < 0.0) return false;
    const double toRad = (fromDegrees + 180.0) * kDegToRad;
    u = speedMps * std::sin(toRad);
    v = speedMps * std::cos(toRad);
    return std::isfinite(u) && std::isfinite(v);
}

void uvToFrom(const double u, const double v, double& fromDegrees, double& speedMps) {
    speedMps = std::hypot(u, v);
    fromDegrees = std::fmod(std::atan2(-u, -v) * kRadToDeg + 360.0, 360.0);
}

}  // namespace

BunkersMotion estimateBunkersMotion(const std::vector<VadLevel>& levels,
                                    const double radarElevationMeters,
                                    const BunkersConfig config) {
    BunkersMotion motion;
    double meanU = 0.0, meanV = 0.0, meanW = 0.0;
    double lowerU = 0.0, lowerV = 0.0, lowerW = 0.0;
    double upperU = 0.0, upperV = 0.0, upperW = 0.0;
    int qualifying = 0;
    for (const auto& level : levels) {
        if (level.quality != VadQuality::Good && level.quality != VadQuality::Marginal)
            continue;
        if (!std::isfinite(level.heightMslMeters) || !std::isfinite(level.directionFromDegrees) ||
            !std::isfinite(level.speedMetersPerSecond))
            continue;
        const double agl = level.heightMslMeters - radarElevationMeters;
        if (!std::isfinite(agl) || agl < 0.0 || agl > config.layerTopAglMeters) continue;
        double u = 0.0, v = 0.0;
        if (!fromToUV(level.directionFromDegrees, level.speedMetersPerSecond, u, v)) continue;
        const double weight = static_cast<double>(level.gateCount > 0 ? level.gateCount : 1);
        meanU += u * weight;
        meanV += v * weight;
        meanW += weight;
        if (agl <= config.lowerTopAglMeters) {
            lowerU += u * weight;
            lowerV += v * weight;
            lowerW += weight;
        } else if (agl >= config.upperBottomAglMeters) {
            upperU += u * weight;
            upperV += v * weight;
            upperW += weight;
        }
        ++qualifying;
    }
    if (qualifying < 2 || meanW <= 0.0 || lowerW <= 0.0 || upperW <= 0.0) return motion;
    meanU /= meanW;
    meanV /= meanW;
    lowerU /= lowerW;
    lowerV /= lowerW;
    upperU /= upperW;
    upperV /= upperW;
    const double shearU = upperU - lowerU;
    const double shearV = upperV - lowerV;
    motion.shearMagnitudeMps = std::hypot(shearU, shearV);
    if (!std::isfinite(motion.shearMagnitudeMps)) return motion;
    motion.valid = true;
    motion.quality = qualifying >= 4 ? VadQuality::Good : VadQuality::Marginal;
    uvToFrom(meanU, meanV, motion.meanFromDegrees, motion.meanSpeedMps);
    if (motion.shearMagnitudeMps < config.weakShearMps) {
        motion.weakShear = true;
        motion.rightFromDegrees = motion.meanFromDegrees;
        motion.rightSpeedMps = motion.meanSpeedMps;
        motion.leftFromDegrees = motion.meanFromDegrees;
        motion.leftSpeedMps = motion.meanSpeedMps;
        return motion;
    }
    motion.weakShear = false;
    // Right of the shear vector (clockwise): (x, y) -> (-y, x) is
    // counterclockwise in math axes (y north), so right = (y, -x).
    const double unitU = shearU / motion.shearMagnitudeMps;
    const double unitV = shearV / motion.shearMagnitudeMps;
    const double rightU = meanU + config.deviationMps * unitV;
    const double rightV = meanV - config.deviationMps * unitU;
    const double leftU = meanU - config.deviationMps * unitV;
    const double leftV = meanV + config.deviationMps * unitU;
    uvToFrom(rightU, rightV, motion.rightFromDegrees, motion.rightSpeedMps);
    uvToFrom(leftU, leftV, motion.leftFromDegrees, motion.leftSpeedMps);
    return motion;
}

}  // namespace cappedweather
