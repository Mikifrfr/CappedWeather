#pragma once

#include "core/level2_decoder.hpp"

#include <optional>

namespace cappedweather {

struct ScanDiagnostics {
    int rayCount = 0;
    int tiltCount = 0;
    double azimuthCoverageDegrees = 0.0;
    double largestAzimuthGapDegrees = 360.0;
    double maximumRangeMeters = 0.0;
};

ScanDiagnostics analyzeScan(const DecodedChunk& chunk, ProductKind product,
                            std::optional<int> elevationNumber = std::nullopt);

} // namespace cappedweather
