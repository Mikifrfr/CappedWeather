#include "core/vad.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cappedweather {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kMinVadGates = 25;
constexpr double kMinVadCoverageDegrees = 90.0;

// Least-squares thresholds: Good needs a dense full circle with a tight
// residual; Marginal tolerates half the circle or moderate noise. Anything
// solved but worse is Poor (displayed with its RMSE, never silently
// dropped), and anything unsolvable is Invalid (fail closed).
constexpr std::size_t kGoodGateCount = 200;
constexpr double kGoodCoverageDegrees = 270.0;
constexpr double kGoodRmse = 3.0;
constexpr std::size_t kMarginalGateCount = 60;
constexpr double kMarginalCoverageDegrees = 180.0;
constexpr double kMarginalRmse = 6.0;

double coverageFromAzimuths(std::vector<double> azimuths) {
    if (azimuths.empty()) return 0.0;
    for (auto& azimuth : azimuths) {
        azimuth = std::fmod(azimuth, 360.0);
        if (azimuth < 0.0) azimuth += 360.0;
    }
    std::sort(azimuths.begin(), azimuths.end());
    double largestGap = 0.0;
    for (std::size_t i = 1; i < azimuths.size(); ++i)
        largestGap = std::max(largestGap, azimuths[i] - azimuths[i - 1]);
    largestGap = std::max(largestGap, azimuths.front() + 360.0 - azimuths.back());
    return 360.0 - largestGap;
}

}  // namespace

std::vector<VadLevel> fitVadProfile(const DecodedChunk& chunk, const ProcessedVelocity& dealiased,
                                    double radarElevationMeters) {
    const auto velocityIndex = static_cast<std::size_t>(ProductKind::Velocity);
    std::vector<VadLevel> levels;
    if (chunk.rays.empty() || dealiased.size() != chunk.rays.size()) return levels;

    // Tilts in first-seen order (matches the per-tilt table ordering).
    std::vector<int> tiltOrder;
    for (const auto& ray : chunk.rays) {
        if (!ray.has(ProductKind::Velocity)) continue;
        if (std::find(tiltOrder.begin(), tiltOrder.end(), ray.elevationNumber) == tiltOrder.end())
            tiltOrder.push_back(ray.elevationNumber);
    }

    for (const int tilt : tiltOrder) {
        VadLevel level;
        level.elevationNumber = tilt;
        // Normal-equation sums for Vr = a + b*sin(az) + c*cos(az).
        double sumS = 0.0, sumC = 0.0, sumSS = 0.0, sumCC = 0.0, sumSC = 0.0;
        double sumV = 0.0, sumVS = 0.0, sumVC = 0.0;
        double elevationSum = 0.0;
        std::size_t rayCount = 0;
        std::vector<double> usedAzimuths;
        std::vector<double> heightSamples;
        for (std::size_t i = 0; i < chunk.rays.size(); ++i) {
            const auto& ray = chunk.rays[i];
            if (ray.elevationNumber != tilt || !ray.has(ProductKind::Velocity)) continue;
            if (i >= dealiased.size() || dealiased[i].empty()) continue;
            const double azimuthRad = static_cast<double>(ray.azimuthDegrees) * kPi / 180.0;
            const double sinAz = std::sin(azimuthRad);
            const double cosAz = std::cos(azimuthRad);
            bool rayUsed = false;
            const auto& gates = dealiased[i];
            for (std::size_t gate = 0; gate < gates.size(); ++gate) {
                const double value = gates[gate];
                if (!std::isfinite(value)) continue;
                sumS += sinAz;
                sumC += cosAz;
                sumSS += sinAz * sinAz;
                sumCC += cosAz * cosAz;
                sumSC += sinAz * cosAz;
                sumV += value;
                sumVS += value * sinAz;
                sumVC += value * cosAz;
                ++level.gateCount;
                rayUsed = true;
                // Every 16th gate is plenty for a median height sample and
                // keeps large tilts out of an MxN memory blowup.
                if (gate % 16 == 0) {
                    const double slant = gateCenterSlantRange(
                        ray.firstGateByProduct.at(velocityIndex),
                        ray.gateSpacingByProduct.at(velocityIndex), gate);
                    if (slant > 0.0) {
                        heightSamples.push_back(
                            standardBeamGeometry(slant, ray.elevationDegrees)
                                .heightAboveRadarMeters);
                    }
                }
            }
            if (rayUsed) {
                usedAzimuths.push_back(ray.azimuthDegrees);
                elevationSum += ray.elevationDegrees;
                ++rayCount;
            }
        }
        if (rayCount == 0) continue;  // tilt without usable velocity: no row
        level.elevationDegrees = elevationSum / static_cast<double>(rayCount);
        level.azimuthCoverageDegrees = coverageFromAzimuths(usedAzimuths);
        if (!heightSamples.empty()) {
            auto middle = heightSamples.begin() + heightSamples.size() / 2;
            std::nth_element(heightSamples.begin(), middle, heightSamples.end());
            level.heightMslMeters = *middle + radarElevationMeters;
        }

        const double n = static_cast<double>(level.gateCount);
        // Cramer's rule on the 3x3 normal equations. A degenerate azimuth
        // spread (single ray, narrow sector) makes the determinant vanish,
        // which fails closed to Invalid below instead of a phantom wind.
        const double det = n * (sumSS * sumCC - sumSC * sumSC) -
                           sumS * (sumS * sumCC - sumSC * sumC) +
                           sumC * (sumS * sumSC - sumSS * sumC);
        bool solved = std::isfinite(det) && det != 0.0;
        double a = 0.0, b = 0.0, c = 0.0;
        if (solved) {
            a = (sumV * (sumSS * sumCC - sumSC * sumSC) - sumS * (sumVS * sumCC - sumSC * sumVC) +
                 sumC * (sumVS * sumSC - sumSS * sumVC)) /
                det;
            b = (n * (sumVS * sumCC - sumSC * sumVC) - sumV * (sumS * sumCC - sumSC * sumC) +
                 sumC * (sumS * sumVC - sumVS * sumC)) /
                det;
            c = (n * (sumSS * sumVC - sumVS * sumSC) - sumS * (sumS * sumVC - sumVS * sumC) +
                 sumV * (sumS * sumSC - sumSS * sumC)) /
                det;
            solved = std::isfinite(a) && std::isfinite(b) && std::isfinite(c);
        }
        if (!solved || level.gateCount < kMinVadGates ||
            level.azimuthCoverageDegrees < kMinVadCoverageDegrees) {
            level.quality = VadQuality::Invalid;
            levels.push_back(level);
            continue;
        }
        // Second pass: residual RMSE of the solved fit.
        double sumSquaredResidual = 0.0;
        for (std::size_t i = 0; i < chunk.rays.size(); ++i) {
            const auto& ray = chunk.rays[i];
            if (ray.elevationNumber != tilt || !ray.has(ProductKind::Velocity)) continue;
            if (i >= dealiased.size()) continue;
            const double azimuthRad = static_cast<double>(ray.azimuthDegrees) * kPi / 180.0;
            const double predicted = a + b * std::sin(azimuthRad) + c * std::cos(azimuthRad);
            for (const float gateValue : dealiased[i]) {
                if (!std::isfinite(gateValue)) continue;
                const double residual = static_cast<double>(gateValue) - predicted;
                sumSquaredResidual += residual * residual;
            }
        }
        level.rmseMetersPerSecond = std::sqrt(sumSquaredResidual / n);
        const double elevationRad = level.elevationDegrees * kPi / 180.0;
        const double cosElevation = std::cos(elevationRad);
        if (!(cosElevation > 1e-3) || !std::isfinite(level.rmseMetersPerSecond)) {
            level.quality = VadQuality::Invalid;
            levels.push_back(level);
            continue;
        }
        level.speedMetersPerSecond = std::hypot(b, c) / cosElevation;
        // atan2(b, c) is the TOWARD direction (azimuth convention: Vr grows
        // with sin on east, cos on north). Meteorology reports FROM.
        double toward = std::atan2(b, c) * 180.0 / kPi;
        if (toward < 0.0) toward += 360.0;
        level.directionFromDegrees = std::fmod(toward + 180.0, 360.0);
        if (!std::isfinite(level.speedMetersPerSecond) ||
            !std::isfinite(level.directionFromDegrees)) {
            level.quality = VadQuality::Invalid;
        } else if (level.gateCount >= kGoodGateCount &&
                   level.azimuthCoverageDegrees >= kGoodCoverageDegrees &&
                   level.rmseMetersPerSecond <= kGoodRmse) {
            level.quality = VadQuality::Good;
        } else if (level.gateCount >= kMarginalGateCount &&
                   level.azimuthCoverageDegrees >= kMarginalCoverageDegrees &&
                   level.rmseMetersPerSecond <= kMarginalRmse) {
            level.quality = VadQuality::Marginal;
        } else {
            level.quality = VadQuality::Poor;
        }
        levels.push_back(level);
    }
    return levels;
}

}  // namespace cappedweather
