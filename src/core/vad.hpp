#pragma once

// VAD (Velocity Azimuth Display) layer-mean wind: a least-squares sine fit
// Vr(az) = a + b*sin(az) + c*cos(az) on dealiased velocity gates, per tilt.
// Horizontal wind follows from (b, c): speed = hypot(b,c)/cos(elevation),
// direction = atan2(b,c). Guidance only, never warnings: every fit carries
// a quality flag, and degenerate geometry (singular matrix, few gates,
// narrow azimuth coverage, NaN input) fails closed to Invalid instead of
// emitting a phantom wind.

#include "core/level2_decoder.hpp"
#include "core/velocity_processing.hpp"

#include <cstddef>
#include <limits>
#include <vector>

namespace cappedweather {

enum class VadQuality {
    Good,      // dense gates, full-circle coverage, low RMSE
    Marginal,  // usable with caveats (sparse, partial circle, or noisy)
    Poor,      // fit solved but untrustworthy (shown, not hidden)
    Invalid,   // no fit: singular, too few gates, or no velocity data
};

struct VadLevel {
    int elevationNumber = 0;
    double elevationDegrees = 0.0;
    double heightMslMeters = std::numeric_limits<double>::quiet_NaN();
    // Meteorological FROM direction (deg, 0-360) plus speed (m/s).
    double directionFromDegrees = std::numeric_limits<double>::quiet_NaN();
    double speedMetersPerSecond = std::numeric_limits<double>::quiet_NaN();
    double rmseMetersPerSecond = std::numeric_limits<double>::quiet_NaN();
    std::size_t gateCount = 0;
    double azimuthCoverageDegrees = 0.0;
    VadQuality quality = VadQuality::Invalid;
};

// One row per tilt that carries velocity moments. `dealiased` must align
// with chunk.rays (processVelocity output, Dealiased mode); non-finite
// gates are skipped. radarElevationMeters anchors the reported MSL height.
std::vector<VadLevel> fitVadProfile(const DecodedChunk& chunk, const ProcessedVelocity& dealiased,
                                    double radarElevationMeters = 0.0);

}  // namespace cappedweather
