#pragma once

#include "core/level2_decoder.hpp"
#include "core/lightning_overlay.hpp"
#include "core/velocity_processing.hpp"

#include <cstddef>
#include <vector>

namespace cappedweather {

struct RadarFeatureConfig {
    double gridSizeDeg = 0.03;  // ~3 km cells: meso-scale, not storm-scale.
    // Was 0.05 (~5 km): TVS couplets fell inside one cell so min/max
    // collapsed and nothing ever reached the rotating gate (0/68 vols).
    double minimumReflectivityDbz = 35.0;
    double minimumVelocitySpanMetersPerSecond = 15.0;  // was 20: folded native
    // velocities underestimated span; dealiased input + lower gate lets real
    // mesos pass while clear-air noise still fails the reflectivity guard.
    double minimumAzimuthalShearPerSecond = 0.0015;  // was 0.0025, computed over
    // a fixed 3 km meso length scale (below) instead of storm area.
    double associationRadiusKm = 45.0;
    int minimumReflectivityCells = 2;
};

struct RadarStormFeature {
    int id = 0;
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double maximumReflectivityDbz = 0.0;
    double velocitySpanMetersPerSecond = 0.0;
    double azimuthalShearPerSecond = 0.0;
    double areaKm2 = 0.0;
    int reflectivityCellCount = 0;
    bool rotating = false;
};

struct LightningRadarAssociation {
    std::size_t lightningCellIndex = 0;
    std::size_t radarFeatureIndex = 0;
    double distanceKm = 0.0;
};

// Extracts low-tilt reflectivity objects and collocated radial-velocity
// couplets from one decoded Level II volume. The result is deliberately
// deterministic and dependency-free so archived and live runs use the same
// fusion path.
//
// When dealiasedVelocity is provided (same layout as processVelocity output),
// velocity min/max come from unfolded gates; otherwise native (folded) values
// are used (legacy path, underestimates span near Nyquist). Always pass the
// dealiased grid in live + replay code.
std::vector<RadarStormFeature> extractRadarStormFeatures(
    const DecodedChunk& chunk, double radarLatitudeDeg, double radarLongitudeDeg,
    RadarFeatureConfig config = {}, const ProcessedVelocity* dealiasedVelocity = nullptr);

std::vector<LightningRadarAssociation> associateLightningWithRadar(
    const std::vector<LightningCellSnapshot>& lightningCells,
    const std::vector<RadarStormFeature>& radarFeatures,
    RadarFeatureConfig config = {});

} // namespace cappedweather
