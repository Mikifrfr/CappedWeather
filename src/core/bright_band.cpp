#include "core/bright_band.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace cappedweather {

namespace {

constexpr std::size_t kGoodGateCount = 2000;
constexpr double kGoodCoverageDegrees = 270.0;
constexpr double kGoodIqrMeters = 750.0;
constexpr std::size_t kMarginalGateCount = 500;
constexpr double kMarginalCoverageDegrees = 180.0;
constexpr double kMarginalIqrMeters = 1500.0;
constexpr std::size_t kMinGates = 60;
constexpr double kMinCoverageDegrees = 90.0;

double ringCoverage(std::vector<double> azimuths) {
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

BrightBandEstimate estimateBrightBand(const DecodedChunk& chunk, const double radarElevationMeters,
                                      const BrightBandConfig config) {
    const auto ccIndex = static_cast<std::size_t>(ProductKind::CorrelationCoefficient);
    const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    BrightBandEstimate estimate;
    std::vector<double> heights;
    std::vector<double> usedAzimuths;
    for (const auto& ray : chunk.rays) {
        if (!ray.has(ProductKind::CorrelationCoefficient) || !ray.has(ProductKind::Reflectivity))
            continue;
        const auto& ccField = ray.fields.at(ccIndex);
        const auto& refField = ray.fields.at(refIndex);
        const auto ccSpacing = ray.gateSpacingByProduct.at(ccIndex);
        const auto refSpacing = ray.gateSpacingByProduct.at(refIndex);
        if (ccSpacing == 0 || refSpacing == 0) continue;
        const auto ccFirst = ray.firstGateByProduct.at(ccIndex);
        const auto refFirst = ray.firstGateByProduct.at(refIndex);
        bool rayUsed = false;
        for (std::size_t gate = 0; gate < ccField.size(); ++gate) {
            const auto cc = nativeValue(ccField[gate], ray.scaleByProduct.at(ccIndex),
                                        ray.offsetByProduct.at(ccIndex));
            if (!cc || !std::isfinite(*cc) || *cc < config.minCorrelationCoefficient ||
                *cc >= config.maxCorrelationCoefficient)
                continue;
            const double slant = gateCenterSlantRange(ccFirst, ccSpacing, gate);
            if (slant <= 0.0) continue;
            const auto refGate = gateIndexForSlantRange(slant, refFirst, refSpacing, refField.size());
            if (!refGate) continue;
            const auto ref = nativeValue(refField.at(*refGate), ray.scaleByProduct.at(refIndex),
                                         ray.offsetByProduct.at(refIndex));
            if (!ref || !std::isfinite(*ref) || *ref < config.minReflectivityDbz ||
                *ref > config.maxReflectivityDbz)
                continue;
            const double height =
                standardBeamGeometry(slant, ray.elevationDegrees).heightAboveRadarMeters +
                radarElevationMeters;
            if (!std::isfinite(height)) continue;
            heights.push_back(height);
            rayUsed = true;
        }
        if (rayUsed) usedAzimuths.push_back(ray.azimuthDegrees);
    }
    estimate.gateCount = heights.size();
    estimate.azimuthCoverageDegrees = ringCoverage(usedAzimuths);
    if (estimate.gateCount < kMinGates ||
        estimate.azimuthCoverageDegrees < kMinCoverageDegrees) {
        return estimate;
    }
    std::sort(heights.begin(), heights.end());
    const auto at = [&](double fraction) {
        return heights.at(static_cast<std::size_t>(fraction * (heights.size() - 1)));
    };
    const double median = at(0.5);
    estimate.heightMslMeters = median;
    estimate.iqrMeters = at(0.75) - at(0.25);
    if (!std::isfinite(median) || !std::isfinite(estimate.iqrMeters)) return estimate;
    estimate.valid = true;
    if (estimate.gateCount >= kGoodGateCount &&
        estimate.azimuthCoverageDegrees >= kGoodCoverageDegrees &&
        estimate.iqrMeters <= kGoodIqrMeters) {
        estimate.quality = BrightBandQuality::Good;
    } else if (estimate.gateCount >= kMarginalGateCount &&
               estimate.azimuthCoverageDegrees >= kMarginalCoverageDegrees &&
               estimate.iqrMeters <= kMarginalIqrMeters) {
        estimate.quality = BrightBandQuality::Marginal;
    } else {
        estimate.quality = BrightBandQuality::Poor;
    }
    return estimate;
}

}  // namespace cappedweather
