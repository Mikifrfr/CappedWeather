#pragma once

// NSSL-style circulation detection (TVS / mesocyclone), dependency-free and
// deterministic so live and archived volumes share the path.
//
// This is a heuristic in the spirit of the NWS RPG Tornado Detection
// Algorithm, NOT a port of it: gate-to-gate azimuthal couplets on dealiased
// velocity, range-dependent delta-V gates (beam broadening), vertical
// association across adjacent tilts, then TVS vs mesocyclone classification
// by intensity, diameter, and depth. Thresholds are documented
// approximations; treat markers as guidance, never warnings.
#include "core/level2_decoder.hpp"

#include <cstddef>
#include <vector>

namespace cappedweather {

enum class CirculationKind {
    Mesocyclone,
    TornadoVortex,  // TVS
};

struct CirculationDetection {
    CirculationKind kind = CirculationKind::Mesocyclone;
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double maxDeltaVelocityMetersPerSecond = 0.0;
    double diameterKm = 0.0;
    double depthMeters = 0.0;
    double baseMetersMsl = 0.0;
    double topMetersMsl = 0.0;
    int tiltCount = 0;
};

struct CirculationConfig {
    double minGateVelocityMetersPerSecond = 5.0;  // each side must exceed this
    double minReflectivityDbz = 20.0;  // couplet gate must sit in real echo;
    // without this, clear-air noise and static ground clutter (which repeats
    // every volume, defeating time persistence) generate bogus couplets.
    int minRunGates = 2;  // range-adjacent couplet gates to form a candidate
    double mesoBaseDeltaV = 10.0;                 // m/s at 0 km range
    double mesoRangeSlope = 0.06;                 // +m/s per km of range
    double tvsBaseDeltaV = 25.0;
    double tvsRangeSlope = 0.08;
    double tvsMaxDiameterKm = 3.0;    // TVS must be compact
    double minDepthTvsMeters = 2500.0;
    double minDepthMesoMeters = 3000.0;
    double verticalAlignKm = 5.0;     // horizontal tolerance between tilts
    int minTilts = 2;                 // vertical continuity requirement
    int maxGateGap = 2;               // range clustering tolerance (gates)
    int maxDetections = 24;
};

// Dealiased velocity grid aligned with chunk.rays and each ray's native
// velocity gates (see processVelocity); NaN marks missing/folded gates.
// Runs fully off the UI thread; pure function of its inputs.
std::vector<CirculationDetection> detectCirculations(
    const DecodedChunk& chunk, const std::vector<std::vector<float>>& dealiasedVelocity,
    double radarLatitudeDeg, double radarLongitudeDeg, double radarElevationMeters,
    CirculationConfig config = {});

}  // namespace cappedweather
