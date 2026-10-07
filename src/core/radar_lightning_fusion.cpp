#include "core/radar_lightning_fusion.hpp"

#include "core/product.hpp"
#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace cappedweather {
namespace {

struct GridCell {
    double weightedLatitude = 0.0;
    double weightedLongitude = 0.0;
    double reflectivityWeight = 0.0;
    double maximumReflectivity = -std::numeric_limits<double>::infinity();
    double minimumVelocity = std::numeric_limits<double>::infinity();
    double maximumVelocity = -std::numeric_limits<double>::infinity();
    int reflectivitySamples = 0;
    int velocitySamples = 0;
};

double cellAreaKm2(const double latitudeDeg, const double gridSizeDeg) {
    constexpr double kKmPerDegree = 111.195;
    return gridSizeDeg * kKmPerDegree *
           gridSizeDeg * kKmPerDegree *
           std::max(0.05, std::cos(latitudeDeg * std::numbers::pi / 180.0));
}

} // namespace

std::vector<RadarStormFeature> extractRadarStormFeatures(
    const DecodedChunk& chunk, const double radarLatitudeDeg, const double radarLongitudeDeg,
    const RadarFeatureConfig config, const ProcessedVelocity* dealiasedVelocity) {
    // Split-cut aware: NEXRAD VCPs put reflectivity-only surveillance (#1) below
    // the lowest Doppler tilt (#2). The old code took the single lowest
    // elevation carrying either product (#1, no velocity) so velocitySamples
    // was always 0 and ROT fired 0/68 volumes. Take each product from its own
    // lowest tilt; both are ~0.5 deg so the geographic mix is sound.
    if (chunk.rays.empty() || config.gridSizeDeg <= 0.0) return {};

    std::uint8_t lowRefl = std::numeric_limits<std::uint8_t>::max();
    std::uint8_t lowVel = std::numeric_limits<std::uint8_t>::max();
    for (const auto& ray : chunk.rays) {
        if (ray.has(ProductKind::Reflectivity)) lowRefl = std::min(lowRefl, ray.elevationNumber);
        if (ray.has(ProductKind::Velocity)) lowVel = std::min(lowVel, ray.elevationNumber);
    }
    if (lowRefl == std::numeric_limits<std::uint8_t>::max() &&
        lowVel == std::numeric_limits<std::uint8_t>::max())
        return {};

    std::unordered_map<std::int64_t, GridCell> cells;
    const GeographicCoordinate origin{radarLatitudeDeg, radarLongitudeDeg};
    for (std::size_t rayIndex = 0; rayIndex < chunk.rays.size(); ++rayIndex) {
        const auto& ray = chunk.rays[rayIndex];
        for (const auto product : {ProductKind::Reflectivity, ProductKind::Velocity}) {
            if (!ray.has(product)) continue;
            // Each product comes from its own lowest tilt (split cuts).
            if (product == ProductKind::Reflectivity && ray.elevationNumber != lowRefl) continue;
            if (product == ProductKind::Velocity && ray.elevationNumber != lowVel) continue;
            const auto productIndex = static_cast<std::size_t>(product);
            const auto spacing = ray.gateSpacingByProduct.at(productIndex);
            const auto firstGate = ray.firstGateByProduct.at(productIndex);
            if (spacing == 0) continue;
            const bool useDealiased =
                product == ProductKind::Velocity && dealiasedVelocity != nullptr &&
                rayIndex < dealiasedVelocity->size();
            const auto& values = ray.fields.at(productIndex);
            const std::size_t gateCount =
                useDealiased ? (*dealiasedVelocity)[rayIndex].size() : values.size();
            for (std::size_t gate = 0; gate < gateCount; ++gate) {
                double gateValue = 0.0;
                if (useDealiased) {
                    const float v = (*dealiasedVelocity)[rayIndex][gate];
                    if (!std::isfinite(v)) continue;
                    gateValue = static_cast<double>(v);
                } else {
                    if (gate >= values.size()) continue;
                    const auto value = nativeValue(values[gate], ray.scaleByProduct.at(productIndex),
                                                   ray.offsetByProduct.at(productIndex));
                    if (!value) continue;
                    gateValue = *value;
                }
                if (product == ProductKind::Reflectivity && gateValue < config.minimumReflectivityDbz)
                    continue;
                const double slantRange = gateCenterSlantRange(firstGate, spacing, gate);
                if (slantRange <= 0.0 || slantRange > 300000.0) continue;
                const auto beam = standardBeamGeometry(slantRange, ray.elevationDegrees);
                const auto coordinate = destinationPoint(origin, ray.azimuthDegrees,
                                                         beam.surfaceDistanceMeters);
                const int x = static_cast<int>(std::floor(coordinate.longitude / config.gridSizeDeg));
                const int y = static_cast<int>(std::floor(coordinate.latitude / config.gridSizeDeg));
                auto& cell = cells[gridCellKey(x, y)];
                if (product == ProductKind::Reflectivity) {
                    const double weight = std::max(1.0, gateValue - config.minimumReflectivityDbz + 1.0);
                    cell.weightedLatitude += coordinate.latitude * weight;
                    cell.weightedLongitude += coordinate.longitude * weight;
                    cell.reflectivityWeight += weight;
                    cell.maximumReflectivity = std::max(cell.maximumReflectivity, gateValue);
                    ++cell.reflectivitySamples;
                } else {
                    cell.minimumVelocity = std::min(cell.minimumVelocity, gateValue);
                    cell.maximumVelocity = std::max(cell.maximumVelocity, gateValue);
                    ++cell.velocitySamples;
                }
            }
        }
    }

    std::unordered_set<std::int64_t> reflectivityKeys;
    for (const auto& [key, cell] : cells) {
        if (cell.reflectivitySamples > 0) reflectivityKeys.insert(key);
    }

    std::vector<RadarStormFeature> features;
    std::unordered_set<std::int64_t> visited;
    int featureId = 1;
    for (const auto seed : reflectivityKeys) {
        if (!visited.insert(seed).second) continue;
        std::queue<std::int64_t> pending;
        pending.push(seed);
        std::vector<std::int64_t> component;
        while (!pending.empty()) {
            const auto key = pending.front();
            pending.pop();
            component.push_back(key);
            int x = 0;
            int y = 0;
            unpackGridCellKey(key, x, y);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const auto neighbor = gridCellKey(x + dx, y + dy);
                    if (reflectivityKeys.contains(neighbor) && visited.insert(neighbor).second)
                        pending.push(neighbor);
                }
            }
        }
        if (static_cast<int>(component.size()) < config.minimumReflectivityCells) continue;

        RadarStormFeature feature;
        feature.id = featureId++;
        feature.maximumReflectivityDbz = -std::numeric_limits<double>::infinity();
        double latitudeSum = 0.0;
        double longitudeSum = 0.0;
        double coordinateWeight = 0.0;
        double minimumVelocity = std::numeric_limits<double>::infinity();
        double maximumVelocity = -std::numeric_limits<double>::infinity();
        int velocityCells = 0;
        std::unordered_set<std::int64_t> velocityNeighborhood;
        for (const auto key : component) {
            const auto& cell = cells.at(key);
            latitudeSum += cell.weightedLatitude;
            longitudeSum += cell.weightedLongitude;
            coordinateWeight += cell.reflectivityWeight;
            feature.maximumReflectivityDbz = std::max(feature.maximumReflectivityDbz,
                                                       cell.maximumReflectivity);
            int x = 0;
            int y = 0;
            unpackGridCellKey(key, x, y);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    velocityNeighborhood.insert(gridCellKey(x + dx, y + dy));
        }
        for (const auto key : velocityNeighborhood) {
            const auto it = cells.find(key);
            if (it == cells.end() || it->second.velocitySamples == 0) continue;
            minimumVelocity = std::min(minimumVelocity, it->second.minimumVelocity);
            maximumVelocity = std::max(maximumVelocity, it->second.maximumVelocity);
            ++velocityCells;
        }
        feature.latitudeDeg = coordinateWeight > 0.0 ? latitudeSum / coordinateWeight : 0.0;
        feature.longitudeDeg = coordinateWeight > 0.0 ? longitudeSum / coordinateWeight : 0.0;
        feature.reflectivityCellCount = static_cast<int>(component.size());
        feature.areaKm2 = component.size() * cellAreaKm2(feature.latitudeDeg, config.gridSizeDeg);
        if (velocityCells > 0 && std::isfinite(minimumVelocity) && std::isfinite(maximumVelocity)) {
            feature.velocitySpanMetersPerSecond = maximumVelocity - minimumVelocity;
            // Fixed meso length scale (3 km): the old sqrt(area) scale diluted
            // shear for large storms (span 20 over 15 km -> 0.0013, never
            // rotating), which is why ROT fired 0/68 volumes. Rotation is a
            // local gradient, not storm Span/size.
            constexpr double kMesoScaleMeters = 3000.0;
            feature.azimuthalShearPerSecond = feature.velocitySpanMetersPerSecond /
                                               kMesoScaleMeters;
            feature.rotating = feature.velocitySpanMetersPerSecond >=
                                   config.minimumVelocitySpanMetersPerSecond &&
                               feature.azimuthalShearPerSecond >=
                                   config.minimumAzimuthalShearPerSecond;
        }
        features.push_back(feature);
    }

    std::sort(features.begin(), features.end(), [](const auto& left, const auto& right) {
        if (left.rotating != right.rotating) return left.rotating > right.rotating;
        return left.maximumReflectivityDbz > right.maximumReflectivityDbz;
    });
    for (std::size_t index = 0; index < features.size(); ++index)
        features[index].id = static_cast<int>(index + 1);
    return features;
}

std::vector<LightningRadarAssociation> associateLightningWithRadar(
    const std::vector<LightningCellSnapshot>& lightningCells,
    const std::vector<RadarStormFeature>& radarFeatures,
    const RadarFeatureConfig config) {
    std::vector<LightningRadarAssociation> associations;
    for (std::size_t lightningIndex = 0; lightningIndex < lightningCells.size(); ++lightningIndex) {
        const auto& lightning = lightningCells[lightningIndex];
        double bestDistanceKm = config.associationRadiusKm;
        std::size_t bestFeature = radarFeatures.size();
        for (std::size_t featureIndex = 0; featureIndex < radarFeatures.size(); ++featureIndex) {
            const auto& feature = radarFeatures[featureIndex];
            const auto distance = rangeAndBearing(
                {lightning.latitudeCenterDeg, lightning.longitudeCenterDeg},
                {feature.latitudeDeg, feature.longitudeDeg}).distanceMeters / 1000.0;
            if (distance <= bestDistanceKm) {
                bestDistanceKm = distance;
                bestFeature = featureIndex;
            }
        }
        if (bestFeature < radarFeatures.size())
            associations.push_back({lightningIndex, bestFeature, bestDistanceKm});
    }
    return associations;
}

} // namespace cappedweather
