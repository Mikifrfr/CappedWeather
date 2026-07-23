#pragma once

#include "core/level2_decoder.hpp"
#include "core/lightning_overlay.hpp"

#include <cstddef>
#include <vector>

namespace cappedweather {

struct RadarFeatureConfig {
    double gridSizeDeg = 0.05;
    double minimumReflectivityDbz = 35.0;
    double minimumVelocitySpanMetersPerSecond = 20.0;
    double minimumAzimuthalShearPerSecond = 0.0025;
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
std::vector<RadarStormFeature> extractRadarStormFeatures(
    const DecodedChunk& chunk, double radarLatitudeDeg, double radarLongitudeDeg,
    RadarFeatureConfig config = {});

std::vector<LightningRadarAssociation> associateLightningWithRadar(
    const std::vector<LightningCellSnapshot>& lightningCells,
    const std::vector<RadarStormFeature>& radarFeatures,
    RadarFeatureConfig config = {});

} // namespace cappedweather
