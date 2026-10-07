#pragma once

// Dense LLSD-style azimuthal shear grid (GR-like rotation wash), dependency-free.
//
// The sparse gate-to-gate couplet detector (circulation_detector) needs opposite
// signs and 2+ tilts, so it misses storm-motion-biased and single-tilt shear.
// This grid fits dV/dx (azimuthal derivative) by least squares over a small
// ray x gate window on dealiased velocity, then peaks become ROT-grade
// detections. Runs fully off the UI thread; pure function of its inputs.

#include "core/level2_decoder.hpp"

#include <vector>

namespace cappedweather {

struct ShearGridConfig {
    int azimuthHalfWindow = 1;  // rays each side (3-ray fit by default)
    int rangeHalfWindow = 1;    // gates each side (3x3 = 9 samples max)
    int minSamples = 6;         // need 6/9 finite gates to fit
    double minShearPerSecond = 0.006;  // |dV/dx| gate to score
    double maxResidualMetersPerSecond = 6.0;  // reject folded windows
    double minReflectivityDbz = 35.0;         // same convective-core guard
    double maxRangeKm = 150.0;                // beam broadening kills shear beyond
    double verticalAlignKm = 5.0;
    int minTilts = 2;
    int maxDetections = 24;
};

struct ShearPeak {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double shearPerSecond = 0.0;      // signed dV/dx
    double equivalentDeltaVMps = 0.0;  // |shear| * 3000 m meso scale
    double diameterKm = 0.0;           // azimuthal window width at range
    double depthMeters = 0.0;
    int tiltCount = 0;
};

// Dense cells above threshold (for future wash display); each cell carries its
// tilt elevation so peaks can associate vertically.
struct ShearCell {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double heightMetersAgl = 0.0;
    double rangeKm = 0.0;
    double shearPerSecond = 0.0;
    double residualMps = 0.0;
    int samples = 0;
    double elevationDeg = 0.0;
    std::uint8_t elevationNumber = 0;
};

std::vector<ShearCell> computeShearGrid(
    const DecodedChunk& chunk, const std::vector<std::vector<float>>& dealiasedVelocity,
    double radarLatitudeDeg, double radarLongitudeDeg, ShearGridConfig config = {});

// Local-maximum peaks with vertical association (2+ tilts by default).
std::vector<ShearPeak> detectShearPeaks(
    const DecodedChunk& chunk, const std::vector<std::vector<float>>& dealiasedVelocity,
    double radarLatitudeDeg, double radarLongitudeDeg, double radarElevationMeters,
    ShearGridConfig config = {});

}  // namespace cappedweather
