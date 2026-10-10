#pragma once

// Bunkers storm motion (Bunkers et al. 2000, ID method, documented
// simplification): supercell motion ≈ 0-6 km mean wind deviated 7.5 m/s
// perpendicular to the deep-layer shear vector (right for right-movers).
// Inputs are the VAD profile rows; heights convert MSL->AGL with the radar
// antenna elevation so the 0-6 km window is ground-relative.
//
// Layers: lower = AGL 0-2 km, upper = AGL 4-6 km, mean = AGL 0-6 km,
// gateCount-weighted. Shear = upperMean - lowerMean. Weak shear (< 5 m/s)
// means no split: motion = mean wind with weakShear set. Quality mirrors
// VAD: both halves + >= 4 qualifying levels = Good, both halves + 2-3
// levels = Marginal, anything less = Invalid (never a phantom motion).
// Only Good/Marginal VAD rows enter; Poor rows are shown in the table but
// must not steer storms.
//
// Pure function, no UI, deterministic. Feeds the SRH estimate next.

#include "core/vad.hpp"

#include <vector>

namespace cappedweather {

struct BunkersMotion {
    bool valid = false;
    // 0-6 km mean wind (met FROM deg + m/s).
    double meanFromDegrees = 0.0;
    double meanSpeedMps = 0.0;
    // Right/left-mover storm motion (met FROM deg + m/s). Equal to the mean
    // when shear is weak (weakShear true).
    double rightFromDegrees = 0.0;
    double rightSpeedMps = 0.0;
    double leftFromDegrees = 0.0;
    double leftSpeedMps = 0.0;
    double shearMagnitudeMps = 0.0;
    bool weakShear = true;
    VadQuality quality = VadQuality::Invalid;
};

struct BunkersConfig {
    double lowerTopAglMeters = 2000.0;
    double upperBottomAglMeters = 4000.0;
    double layerTopAglMeters = 6000.0;
    double weakShearMps = 5.0;  // below this the split is noise, not motion
    double deviationMps = 7.5;  // Bunkers et al. 2000 empirical deviation
};

BunkersMotion estimateBunkersMotion(const std::vector<VadLevel>& levels,
                                    double radarElevationMeters,
                                    BunkersConfig config = {});

}  // namespace cappedweather
