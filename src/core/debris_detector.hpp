#pragma once

// Polarimetric tornado debris signature (TDS): a local CC drop collocated
// with a validated rotation couplet column plus significant reflectivity.
// Follows the operational WDTB guidance and the literature consensus
// (Ryzhkov et al. 2005; Schultz et al. 2012; Van Den Broeke climatology):
//
//   - CC (RHOHV) minimum at or below 0.80 in the vortex neighborhood
//   - reflectivity at least 20 dBZ at the low-CC gate (signal quality)
//   - collocation with a TVS/meso detection from detectCirculations
//     (the vortex requirement; we never hunt CC drops alone, because
//     hail cores and melting-layer gradients make those too)
//
// ZDR is recorded as extra confidence only (near 0 dB supports debris,
// but entrainment shifts it, so it never gates). No CC moment in the
// volume (TDWR, single-pol OPERA) means no output, never a guess.
// Guidance only, never a warning: TDS confirms debris already lofted,
// it does not predict tornadogenesis or add warning lead time.

#include "core/circulation_detector.hpp"
#include "core/level2_decoder.hpp"

#include <vector>

namespace cappedweather {

struct TdsDetection {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double minCorrelationCoefficient = 0.0;
    double maxReflectivityDbz = 0.0;
    double meanDifferentialReflectivityDb = 0.0;  // NaN when ZDR absent
    bool hasDifferentialReflectivity = false;
    double parentDeltaVelocityMetersPerSecond = 0.0;
    CirculationKind parentKind = CirculationKind::Mesocyclone;
    double rangeKm = 0.0;
    double heightMslMeters = 0.0;
};

struct TdsConfig {
    double maxCorrelationCoefficient = 0.80;  // Ryzhkov/WDTB debris line
    double minReflectivityDbz = 20.0;  // WDTB signal-quality floor
    double searchRadiusKm = 2.5;  // neighborhood around the vortex fix
    double minContrast = 0.05;  // debris is a compact hole, hail is a broad
    // shield: median CC in the [searchRadius, 2x] annulus must exceed the
    // inner minimum by this much. Screens the rain/hail-mixture CC
    // depressions that drape entire mesocyclones. 0 disables.
    double maxRangeKm = 150.0;  // beam too high / gates too coarse past this
    double maxBeamHeightAglMeters = 4000.0;  // lowest tilt only, low debris
    int maxDetections = 12;
};

// One TDS candidate per parent circulation at most, sorted by ascending
// minimum CC (deepest debris first). Empty when the volume has no CC
// moment or no parent passes the gates.
std::vector<TdsDetection> detectDebrisSignatures(
    const DecodedChunk& chunk, const std::vector<CirculationDetection>& parents,
    double radarLatitudeDeg, double radarLongitudeDeg, double radarElevationMeters,
    TdsConfig config = {});

}  // namespace cappedweather
