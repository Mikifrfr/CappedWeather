#include "core/debris_detector.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace cappedweather {
namespace {

double angularSepDeg(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d < -180.0) d += 360.0;
    if (d > 180.0) d -= 360.0;
    return std::abs(d);
}

}  // namespace

std::vector<TdsDetection> detectDebrisSignatures(
    const DecodedChunk& chunk, const std::vector<CirculationDetection>& parents,
    const double radarLatitudeDeg, const double radarLongitudeDeg,
    const double radarElevationMeters, const TdsConfig config) {
    const auto ccIndex = static_cast<std::size_t>(ProductKind::CorrelationCoefficient);
    const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto zdrIndex = static_cast<std::size_t>(ProductKind::DifferentialReflectivity);

    // Lowest tilt carrying CC: debris detection is a low-level game.
    std::map<std::uint8_t, std::vector<std::size_t>> ccTilts;
    for (std::size_t i = 0; i < chunk.rays.size(); ++i) {
        if (chunk.rays[i].has(ProductKind::CorrelationCoefficient)) ccTilts[chunk.rays[i].elevationNumber].push_back(i);
    }
    if (ccTilts.empty()) return {};
    const auto& ccRays = ccTilts.begin()->second;

    std::vector<TdsDetection> out;
    for (const auto& parent : parents) {
        // NaN vortex fix must fail closed: without this the NaN range
        // bypasses the range gate below (all NaN comparisons are false)
        // and NaN distances read as inner-disc hits.
        if (!std::isfinite(parent.latitudeDeg) || !std::isfinite(parent.longitudeDeg)) continue;
        const auto rb =
            rangeAndBearing({radarLatitudeDeg, radarLongitudeDeg},
                            {parent.latitudeDeg, parent.longitudeDeg});
        if (!std::isfinite(rb.distanceMeters) || !std::isfinite(rb.bearingDegrees)) continue;
        const double rangeKm = rb.distanceMeters / 1000.0;
        if (!std::isfinite(rangeKm) || rangeKm <= 0.0 || rangeKm > config.maxRangeKm) continue;
        // Azimuth window: search disc as seen from the radar, plus margin.
        // Sized for the 2x contrast annulus; the exact disc/annulus split
        // happens per gate below by true distance from the vortex fix.
        const double azTolDeg =
            std::clamp(config.searchRadiusKm * 2.0 / std::max(1.0, rangeKm) * 57.2958, 0.5,
                       12.0) +
            1.0;

        double bestCc = std::numeric_limits<double>::infinity();
        double bestRef = -std::numeric_limits<double>::infinity();
        double bestAz = 0.0;
        double bestSlant = 0.0;
        double bestElev = 0.0;
        double zdrSum = 0.0;
        int zdrCount = 0;
        std::vector<double> outerCc;
        outerCc.reserve(64);

        for (const auto rayIdx : ccRays) {
            const auto& ray = chunk.rays[rayIdx];
            if (angularSepDeg(ray.azimuthDegrees, rb.bearingDegrees) > azTolDeg) continue;
            const auto ccSpacing = ray.gateSpacingByProduct.at(ccIndex);
            if (ccSpacing == 0) continue;
            const auto ccFirst = ray.firstGateByProduct.at(ccIndex);
            const auto& ccField = ray.fields.at(ccIndex);
            const auto refSpacing = ray.gateSpacingByProduct.at(refIndex);
            const auto refFirst = ray.firstGateByProduct.at(refIndex);
            const auto& refField = ray.fields.at(refIndex);
            const bool hasRef = !refField.empty() && refSpacing > 0;
            const bool hasZdr = ray.has(ProductKind::DifferentialReflectivity);
            const auto& zdrField = ray.fields.at(zdrIndex);
            for (std::size_t g = 0; g < ccField.size(); ++g) {
                const double slant = gateCenterSlantRange(ccFirst, ccSpacing, g);
                if (slant <= 0.0) continue;
                const auto beam = standardBeamGeometry(slant, ray.elevationDegrees);
                if (beam.heightAboveRadarMeters > config.maxBeamHeightAglMeters) continue;
                const auto gatePos = destinationPoint({radarLatitudeDeg, radarLongitudeDeg},
                                                      ray.azimuthDegrees,
                                                      beam.surfaceDistanceMeters);
                const double distKm =
                    haversineMeters(gatePos.latitude, gatePos.longitude, parent.latitudeDeg,
                                    parent.longitudeDeg) /
                    1000.0;
                const auto cc = nativeValue(ccField[g], ray.scaleByProduct.at(ccIndex),
                                            ray.offsetByProduct.at(ccIndex));
                if (!cc || *cc < 0.0 || *cc > 1.05) continue;
                // Outer annulus feeds the contrast gate; inner disc feeds
                // the debris minimum. Same tilt, same quality rules.
                if (distKm > config.searchRadiusKm) {
                    if (distKm <= config.searchRadiusKm * 2.0) outerCc.push_back(*cc);
                    continue;
                }
                double ref = -std::numeric_limits<double>::infinity();
                if (hasRef) {
                    if (const auto rg = gateIndexForSlantRange(slant, refFirst, refSpacing,
                                                               refField.size())) {
                        const auto r = nativeValue(refField[*rg],
                                                   ray.scaleByProduct.at(refIndex),
                                                   ray.offsetByProduct.at(refIndex));
                        if (r) ref = *r;
                    }
                }
                if (hasZdr && !zdrField.empty()) {
                    if (const auto zg = gateIndexForSlantRange(
                            slant, ray.firstGateByProduct.at(zdrIndex),
                            ray.gateSpacingByProduct.at(zdrIndex), zdrField.size())) {
                        const auto z = nativeValue(zdrField[*zg],
                                                   ray.scaleByProduct.at(zdrIndex),
                                                   ray.offsetByProduct.at(zdrIndex));
                        if (z) {
                            zdrSum += *z;
                            ++zdrCount;
                        }
                    }
                }
                if (*cc < bestCc) {
                    bestCc = *cc;
                    bestRef = ref;
                    bestAz = ray.azimuthDegrees;
                    bestSlant = slant;
                    bestElev = ray.elevationDegrees;
                }
            }
        }
        if (!std::isfinite(bestCc) || bestCc > config.maxCorrelationCoefficient) continue;
        if (!std::isfinite(bestRef) || bestRef < config.minReflectivityDbz) continue;
        if (config.minContrast > 0.0) {
            // Debris is a compact hole: surroundings must read clearly more
            // meteorological. A broad hail-shield depression fails here.
            std::sort(outerCc.begin(), outerCc.end());
            const double medianOuter =
                outerCc.empty() ? std::numeric_limits<double>::quiet_NaN()
                                : outerCc[outerCc.size() / 2];
            if (!std::isfinite(medianOuter) || medianOuter - bestCc < config.minContrast)
                continue;
        }
        const auto beam = standardBeamGeometry(bestSlant, bestElev);
        const auto pos = destinationPoint({radarLatitudeDeg, radarLongitudeDeg}, bestAz,
                                          beam.surfaceDistanceMeters);
        TdsDetection det;
        det.latitudeDeg = pos.latitude;
        det.longitudeDeg = pos.longitude;
        det.minCorrelationCoefficient = bestCc;
        det.maxReflectivityDbz = bestRef;
        if (zdrCount > 0) {
            det.meanDifferentialReflectivityDb = zdrSum / zdrCount;
            det.hasDifferentialReflectivity = true;
        } else {
            det.meanDifferentialReflectivityDb = std::numeric_limits<double>::quiet_NaN();
        }
        det.parentDeltaVelocityMetersPerSecond = parent.maxDeltaVelocityMetersPerSecond;
        det.parentKind = parent.kind;
        det.rangeKm = rangeKm;
        det.heightMslMeters = radarElevationMeters + beam.heightAboveRadarMeters;
        out.push_back(det);
    }

    std::sort(out.begin(), out.end(), [](const TdsDetection& a, const TdsDetection& b) {
        return a.minCorrelationCoefficient < b.minCorrelationCoefficient;
    });
    if (static_cast<int>(out.size()) > config.maxDetections)
        out.resize(static_cast<std::size_t>(config.maxDetections));
    return out;
}

}  // namespace cappedweather
