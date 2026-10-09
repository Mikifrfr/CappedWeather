#pragma once

// Bright band (melting layer): stratiform rain shows a quasi-circular ring
// of lowered correlation coefficient where snow melts, all at nearly the
// same height. The median height of qualifying gates estimates the
// freezing level per volume, which later gates ZDR columns on the measured
// melting level instead of a fixed 3 km.
//
// Gate rule: 0.80 <= CC < 0.97 with 15 <= REF <= 45 dBZ (stratiform echo;
// convective cores and debris are excluded by the REF window, ground
// clutter by the CC floor). Quality mirrors VAD: dense full-circle rings
// with a tight height IQR are Good; sparse or smeared fits are Poor and
// stay visible rather than vanishing.
//
// Pure function, no UI, deterministic. NaN/inf gates are skipped.

#include "core/level2_decoder.hpp"

#include <cstddef>
#include <limits>
#include <vector>

namespace cappedweather {

enum class BrightBandQuality {
    Good,
    Marginal,
    Poor,
    Invalid,
};

struct BrightBandEstimate {
    bool valid = false;
    double heightMslMeters = std::numeric_limits<double>::quiet_NaN();
    double iqrMeters = std::numeric_limits<double>::quiet_NaN();
    std::size_t gateCount = 0;
    double azimuthCoverageDegrees = 0.0;
    BrightBandQuality quality = BrightBandQuality::Invalid;
};

struct BrightBandConfig {
    double minCorrelationCoefficient = 0.80;
    double maxCorrelationCoefficient = 0.97;
    double minReflectivityDbz = 15.0;
    double maxReflectivityDbz = 45.0;
};

BrightBandEstimate estimateBrightBand(const DecodedChunk& chunk, double radarElevationMeters,
                                      BrightBandConfig config = {});

}  // namespace cappedweather
