#pragma once

#include "core/level2_decoder.hpp"

#include <optional>
#include <vector>

namespace cappedweather {

// Per-tilt breakdown: {elevationNumber, rayCount, maxGap}.
struct TiltDiagnostics {
    int elevationNumber = 0;
    int rayCount = 0;
    double largestAzimuthGapDegrees = 360.0;
};

struct ScanDiagnostics {
    int rayCount = 0;
    int tiltCount = 0;
    double azimuthCoverageDegrees = 0.0;
    double largestAzimuthGapDegrees = 360.0;
    double maximumRangeMeters = 0.0;
    std::vector<TiltDiagnostics> perTilt;
};

ScanDiagnostics analyzeScan(const DecodedChunk& chunk, ProductKind product,
                            std::optional<int> elevationNumber = std::nullopt);
// Overload that also reports the per-tilt breakdown via the out parameter
// (mirrors result.perTilt). Existing 2/3-arg calls keep binding to the
// overload above, so no call site breaks.
ScanDiagnostics analyzeScan(const DecodedChunk& chunk, ProductKind product,
                            std::vector<TiltDiagnostics>& perTiltOut,
                            std::optional<int> elevationNumber = std::nullopt);

} // namespace cappedweather
