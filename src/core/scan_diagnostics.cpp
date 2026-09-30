#include "core/scan_diagnostics.hpp"
#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace cappedweather {
namespace {

double largestGapDegrees(std::vector<double>& azimuths) {
    if (azimuths.empty()) return 360.0;
    std::sort(azimuths.begin(), azimuths.end());
    double gap = 0.0;
    for (std::size_t index = 0; index < azimuths.size(); ++index) {
        const auto next =
            index + 1 < azimuths.size() ? azimuths[index + 1] : azimuths.front() + 360.0;
        gap = std::max(gap, next - azimuths[index]);
    }
    return gap;
}

ScanDiagnostics analyzeImpl(const DecodedChunk& chunk, const ProductKind product,
                            const std::optional<int> elevationNumber,
                            std::vector<TiltDiagnostics>* perTiltOut) {
    ScanDiagnostics result;
    std::vector<double> azimuths;
    std::set<int> tilts;
    std::map<int, std::vector<double>> azimuthsByTilt;
    std::map<int, int> countsByTilt;
    const auto productIndex = static_cast<std::size_t>(product);
    for (const auto& ray : chunk.rays) {
        if (elevationNumber && ray.elevationNumber != *elevationNumber) continue;
        if (!ray.has(product)) continue;
        ++result.rayCount;
        tilts.insert(ray.elevationNumber);
        auto azimuth = std::fmod(static_cast<double>(ray.azimuthDegrees) + 360.0, 360.0);
        azimuths.push_back(azimuth);
        azimuthsByTilt[ray.elevationNumber].push_back(azimuth);
        ++countsByTilt[ray.elevationNumber];
        const auto& values = ray.fields.at(productIndex);
        const auto spacing = ray.gateSpacingByProduct.at(productIndex);
        if (!values.empty() && spacing > 0) {
            const auto outerEdge = gateCenterSlantRange(
                                       ray.firstGateByProduct.at(productIndex), spacing,
                                       values.size() - 1) +
                                   spacing * 0.5;
            result.maximumRangeMeters = std::max(result.maximumRangeMeters, outerEdge);
        }
    }
    result.tiltCount = static_cast<int>(tilts.size());
    if (!azimuths.empty()) {
        result.largestAzimuthGapDegrees = largestGapDegrees(azimuths);
        result.azimuthCoverageDegrees =
            std::clamp(360.0 - result.largestAzimuthGapDegrees, 0.0, 360.0);
    }
    result.perTilt.reserve(azimuthsByTilt.size());
    for (auto& [elevation, tiltAzimuths] : azimuthsByTilt) {
        TiltDiagnostics tilt;
        tilt.elevationNumber = elevation;
        tilt.rayCount = countsByTilt[elevation];
        tilt.largestAzimuthGapDegrees = largestGapDegrees(tiltAzimuths);
        result.perTilt.push_back(tilt);
    }
    if (perTiltOut != nullptr) *perTiltOut = result.perTilt;
    return result;
}

} // namespace

ScanDiagnostics analyzeScan(const DecodedChunk& chunk, const ProductKind product,
                            const std::optional<int> elevationNumber) {
    return analyzeImpl(chunk, product, elevationNumber, nullptr);
}

ScanDiagnostics analyzeScan(const DecodedChunk& chunk, const ProductKind product,
                            std::vector<TiltDiagnostics>& perTiltOut,
                            const std::optional<int> elevationNumber) {
    return analyzeImpl(chunk, product, elevationNumber, &perTiltOut);
}

} // namespace cappedweather
