#pragma once

#include "core/level2_decoder.hpp"

#include <vector>

namespace cappedweather {

enum class VelocityProcessingMode {
    Native,
    Dealiased,
    StormRelative,
};

struct VelocityProcessingOptions {
    VelocityProcessingMode mode = VelocityProcessingMode::Native;
    double nyquistVelocityMetersPerSecond = 25.0;
    double stormMotionDirectionDegrees = 0.0; // Direction toward which the storm moves.
    double stormMotionSpeedMetersPerSecond = 0.0;
};

// Output is aligned with DecodedChunk::rays and each ray's native velocity gates.
// Missing and range-folded gates are represented by NaN.
using ProcessedVelocity = std::vector<std::vector<float>>;

ProcessedVelocity processVelocity(const DecodedChunk& chunk, VelocityProcessingOptions options);

} // namespace cappedweather
