#include "core/scan_diagnostics.hpp"
#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace cappedweather {

ScanDiagnostics analyzeScan(const DecodedChunk& chunk, const ProductKind product,
                            const std::optional<int> elevationNumber) {
    ScanDiagnostics result;
    std::vector<double> azimuths;
    std::set<int> tilts;
    const auto productIndex = static_cast<std::size_t>(product);
    for (const auto& ray : chunk.rays) {
        if (elevationNumber && ray.elevationNumber != *elevationNumber) continue;
        if (!ray.has(product)) continue;
        ++result.rayCount;
        tilts.insert(ray.elevationNumber);
        auto azimuth = std::fmod(static_cast<double>(ray.azimuthDegrees) + 360.0, 360.0);
        azimuths.push_back(azimuth);
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
    if (azimuths.empty()) return result;
    std::sort(azimuths.begin(), azimuths.end());
    result.largestAzimuthGapDegrees = 0.0;
    for (std::size_t index = 0; index < azimuths.size(); ++index) {
        const auto next = index + 1 < azimuths.size() ? azimuths[index + 1] : azimuths.front() + 360.0;
        result.largestAzimuthGapDegrees = std::max(result.largestAzimuthGapDegrees, next - azimuths[index]);
    }
    result.azimuthCoverageDegrees = std::clamp(360.0 - result.largestAzimuthGapDegrees, 0.0, 360.0);
    return result;
}

} // namespace cappedweather
