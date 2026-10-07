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

// Shear watch: a vertically-coherent couplet column (2+ tilts) at
// mesocyclone strength that fails TVS/meso classification (too shallow,
// or TVS-strength but too wide). Gate-to-gate structure, reflectivity
// guard, and over-unfold veto are identical to classified detections, so
// this is the honest rotation-watch tier: real rotation signatures below
// warning-grade confidence. Feeds the map shear overlay (capped and
// decluttered there), never the TVS/meso markers.
struct ShearWatchCandidate {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double maxDeltaVelocityMetersPerSecond = 0.0;
    double diameterKm = 0.0;
    double depthMeters = 0.0;
    int tiltCount = 0;
};

struct CirculationConfig {
    double minGateVelocityMetersPerSecond = 5.0;  // each side must exceed this
    // at close range; relaxed downrange by minGateRangeSlope (beam smoothing
    // weakens far-field velocities). Effective gate = max(2.0, minGate -
    // slope * rangeKm). Mayfield/KHTX sweep: fixed 3.0 doubles far-field hits
    // but floods Moore (63->134 dets, CSI 0.108->0.051); slope keeps Moore
    // clean while helping far field.
    double minGateRangeSlope = 0.02;  // m/s per km; 0 = fixed gate (legacy)
    double minReflectivityDbz = 35.0;  // couplet gate must sit in a convective
    // core, not stratiform echo. Tuned on 68 archived tornadic volumes
    // (KPAH/Moore/Hackleburg, docs/rotation-validation.md): 35 halves false
    // alarms vs 20 at a cost of ~2 warning hits in 243, and tops SPC CSI.
    // Without any guard, clear-air noise and static ground clutter (which
    // repeats every volume, defeating time persistence) generate bogus couplets.
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
    double maxDeltaVelocityMetersPerSecond = 120.0;  // over-unfold veto: gates
    // above this never form runs. Naive neighbor unfolding stacks multiples of
    // 2*Nyquist (e.g. 4x56 = 224 m/s at Nyquist 28); real gate-to-gate TVS
    // shear lives well below 120. Set <= 0 to disable.
    double sameSignBoost = 0.0;  // same-sign (storm-motion-biased) couplets need
    // deltaV >= mesoGate * boost. OFF by default: Moore sweep showed ssb=1.8
    // adds 48 detections with zero new hits (CSI 0.108 -> 0.062); gust-front
    // shear lines dominate. Enable only with a high boost (2.5+) when a case
    // proves storm-motion bias is hiding real mesos. Set <= 0 to disable
    // (opposite-sign only).
    double minSameSignVelocityMetersPerSecond = 3.0;  // each side for same-sign
    bool allowSingleTiltTvs = false;  // escape hatch for one-tilt TVS: requires
    // deltaV >= tvsGate * singleTiltTvsBoost, compact diameter, high reflectivity
    // (already gated). Off by default: single-tilt hits are usually dealias
    // glitches; enable only via sweep when upper tilts are missing/noisy.
    double singleTiltTvsBoost = 1.3;
    int maxDetections = 24;
    int maxShearWatch = 24;  // cap on sub-classification rotation watch
};

// Stage counts for miss diagnosis: populate via the optional out-param to see
// where a volume died (no couplets? depth veto? diameter veto?).
struct CirculationDiagnostics {
    int tiltCoupletCount = 0;   // total couplets across tilts (pre-association)
    int columnCount = 0;        // vertical columns formed
    int singleRejects = 0;      // one-tilt columns held for vertical continuity
    int depthRejects = 0;       // columns failing TVS+meso depth gates
    int weakRejects = 0;        // columns below both delta-V gates
    int diameterRejects = 0;    // TVS-strength but too wide, meso gate missed
    double bestDeltaV = 0.0;    // strongest column best-deltaV seen
    double bestDepthM = 0.0;
};

// Dealiased velocity grid aligned with chunk.rays and each ray's native
// velocity gates (see processVelocity); NaN marks missing/folded gates.
// Runs fully off the UI thread; pure function of its inputs.
//
// shearWatch (optional out-param) receives mesocyclone-strength couplet
// columns that fail TVS/meso classification on depth or diameter. Null by
// default (classification output only).
std::vector<CirculationDetection> detectCirculations(
    const DecodedChunk& chunk, const std::vector<std::vector<float>>& dealiasedVelocity,
    double radarLatitudeDeg, double radarLongitudeDeg, double radarElevationMeters,
    CirculationConfig config = {}, CirculationDiagnostics* diagnostics = nullptr,
    std::vector<ShearWatchCandidate>* shearWatch = nullptr);

}  // namespace cappedweather
