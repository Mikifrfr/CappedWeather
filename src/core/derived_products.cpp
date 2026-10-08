#include "core/derived_products.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace cappedweather {
namespace {

constexpr double kEchoTopThresholdDbz = 18.0;
constexpr double kVilMinimumSignalDbz = 5.0;
constexpr double kVilReflectivityCapDbz = 56.0;
constexpr double kMetersPerKft = 304.8;
constexpr float kDerivedScale = 10.0F;
constexpr float kDerivedOffset = 2.0F;

struct Tilt {
    int elevationNumber = 0;
    double meanElevationDegrees = 0.0;
    std::vector<std::size_t> rayIndexes;
};

struct VerticalSample {
    double heightMslMeters = 0.0;
    double reflectivityDbz = 0.0;
    std::size_t tiltOrdinal = 0;
};

struct GateLookup {
    long long sourceGate = -1;
    double heightMslMeters = 0.0;
};

struct ColumnSource {
    const DecodedRay* ray = nullptr;
    const std::vector<GateLookup>* lookup = nullptr;
    std::size_t tiltOrdinal = 0;
};

using LookupKey = std::tuple<std::int16_t, std::uint16_t, std::size_t, int,
                             std::int16_t, std::uint16_t, int>;

double circularDifference(const double left, const double right) {
    const auto direct = std::abs(left - right);
    return std::min(direct, 360.0 - direct);
}

std::uint16_t encodeDerived(const double value) {
    if (!std::isfinite(value) || value < 0.0) return 0;
    const auto encoded = std::llround(value * kDerivedScale + kDerivedOffset);
    return static_cast<std::uint16_t>(std::clamp<long long>(encoded, 2, 65535));
}

std::optional<std::size_t> nearestRay(const DecodedChunk& chunk, const Tilt& tilt,
                                      const double azimuthDegrees) {
    if (tilt.rayIndexes.empty()) return std::nullopt;
    const auto compare = [&chunk](const std::size_t rayIndex, const double target) {
        return static_cast<double>(chunk.rays.at(rayIndex).azimuthDegrees) < target;
    };
    const auto found = std::lower_bound(tilt.rayIndexes.cbegin(), tilt.rayIndexes.cend(),
                                        azimuthDegrees, compare);
    const auto first = tilt.rayIndexes.cbegin();
    const auto last = tilt.rayIndexes.cend();
    auto best = found == last ? first : found;
    auto bestDifference = circularDifference(chunk.rays.at(*best).azimuthDegrees, azimuthDegrees);
    const auto previous = found == first ? std::prev(last) : std::prev(found);
    const auto previousDifference = circularDifference(chunk.rays.at(*previous).azimuthDegrees,
                                                       azimuthDegrees);
    if (previousDifference < bestDifference) {
        best = previous;
        bestDifference = previousDifference;
    }
    const auto& ray = chunk.rays.at(*best);
    if (bestDifference > std::max(1.5, static_cast<double>(ray.azimuthResolutionDegrees) * 1.5))
        return std::nullopt;
    return *best;
}

std::vector<Tilt> reflectivityTilts(const DecodedChunk& chunk) {
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    std::map<int, std::vector<std::size_t>> grouped;
    for (std::size_t index = 0; index < chunk.rays.size(); ++index) {
        const auto& ray = chunk.rays[index];
        if (!ray.fields.at(reflectivityIndex).empty() &&
            ray.gateSpacingByProduct.at(reflectivityIndex) > 0) {
            grouped[ray.elevationNumber].push_back(index);
        }
    }

    std::vector<Tilt> tilts;
    tilts.reserve(grouped.size());
    for (auto& [number, indexes] : grouped) {
        double totalElevation = 0.0;
        for (const auto index : indexes) totalElevation += chunk.rays.at(index).elevationDegrees;
        std::sort(indexes.begin(), indexes.end(), [&chunk](const auto left, const auto right) {
            return chunk.rays.at(left).azimuthDegrees < chunk.rays.at(right).azimuthDegrees;
        });
        tilts.push_back({number, totalElevation / static_cast<double>(indexes.size()),
                         std::move(indexes)});
    }
    std::sort(tilts.begin(), tilts.end(), [](const Tilt& left, const Tilt& right) {
        if (left.meanElevationDegrees != right.meanElevationDegrees)
            return left.meanElevationDegrees < right.meanElevationDegrees;
        return left.elevationNumber < right.elevationNumber;
    });

    // Supplemental low-level scans can repeat nearly the same elevation.
    // Keep one representative so a duplicate sweep cannot inflate VIL.
    std::vector<Tilt> distinct;
    for (auto& tilt : tilts) {
        if (!distinct.empty() &&
            std::abs(distinct.back().meanElevationDegrees - tilt.meanElevationDegrees) < 0.05) {
            if (tilt.rayIndexes.size() > distinct.back().rayIndexes.size())
                distinct.back() = std::move(tilt);
        } else {
            distinct.push_back(std::move(tilt));
        }
    }
    return distinct;
}

std::vector<GateLookup> buildGateLookup(const std::int16_t outputFirstGate,
                                        const std::uint16_t outputSpacing,
                                        const std::size_t outputGateCount,
                                        const double outputElevationDegrees,
                                        const std::int16_t sourceFirstGate,
                                        const std::uint16_t sourceSpacing,
                                        const double sourceElevationDegrees,
                                        const double radarElevationMeters) {
    std::vector<GateLookup> lookup(outputGateCount);
    for (std::size_t gate = 0; gate < outputGateCount; ++gate) {
        // Message 31 firstGate is the range to the center of bin zero.
        const auto outputSlantRange = gateCenterSlantRange(outputFirstGate, outputSpacing, gate);
        if (outputSlantRange <= 0.0) continue;
        const auto groundRange = standardBeamGeometry(outputSlantRange, outputElevationDegrees)
                                     .surfaceDistanceMeters;
        const auto sourceSlantRange = slantRangeForSurfaceDistance(groundRange,
                                                                    sourceElevationDegrees);
        // Zero spacing divides into inf; non-finite geometry feeds
        // undefined llround below. Leave the gate unmapped instead.
        if (sourceSpacing == 0 || !std::isfinite(sourceSlantRange)) continue;
        const auto sourceGatePosition = (sourceSlantRange - sourceFirstGate) / sourceSpacing;
        if (!std::isfinite(sourceGatePosition)) continue;
        const auto sourceGate = static_cast<long long>(std::llround(sourceGatePosition));
        const auto height = standardBeamGeometry(sourceSlantRange, sourceElevationDegrees)
                                .heightAboveRadarMeters +
                            radarElevationMeters;
        if (!std::isfinite(height)) continue;
        lookup[gate] = {sourceGate, height};
    }
    return lookup;
}

std::optional<std::pair<double, bool>> echoTopMeters(const std::vector<VerticalSample>& samples) {
    std::optional<std::size_t> highestAbove;
    for (std::size_t index = 0; index < samples.size(); ++index)
        if (samples[index].reflectivityDbz >= kEchoTopThresholdDbz) highestAbove = index;
    if (!highestAbove) return std::nullopt;

    const auto index = *highestAbove;
    if (index + 1 >= samples.size())
        return std::pair{samples[index].heightMslMeters, true};
    const auto& below = samples[index];
    const auto& above = samples[index + 1];
    if (above.tiltOrdinal != below.tiltOrdinal + 1 ||
        above.reflectivityDbz >= kEchoTopThresholdDbz ||
        below.reflectivityDbz <= above.reflectivityDbz)
        return std::pair{below.heightMslMeters, true};
    const auto fraction = std::clamp(
        (below.reflectivityDbz - kEchoTopThresholdDbz) /
            (below.reflectivityDbz - above.reflectivityDbz),
        0.0, 1.0);
    return std::pair{below.heightMslMeters +
                         fraction * (above.heightMslMeters - below.heightMslMeters),
                     false};
}

std::optional<double> vilKilogramsPerSquareMeter(const std::vector<VerticalSample>& samples) {    if (samples.size() < 2 ||
        std::none_of(samples.cbegin(), samples.cend(), [](const auto& sample) {
            return sample.reflectivityDbz >= kVilMinimumSignalDbz;
        }))
        return std::nullopt;

    const auto maximumLinearReflectivity = std::pow(10.0, kVilReflectivityCapDbz / 10.0);
    const auto linearReflectivity = [](const double dbz) {
        return std::pow(10.0, dbz / 10.0);
    };
    double integral = 0.0;
    bool integratedLayer = false;
    for (std::size_t index = 1; index < samples.size(); ++index) {
        const auto& lower = samples[index - 1];
        const auto& upper = samples[index];
        if (upper.tiltOrdinal != lower.tiltOrdinal + 1) continue;
        const auto layerDepth = upper.heightMslMeters - lower.heightMslMeters;
        if (layerDepth <= 0.0) continue;
        const auto layerReflectivity = std::min(
            0.5 * (linearReflectivity(lower.reflectivityDbz) +
                   linearReflectivity(upper.reflectivityDbz)),
            maximumLinearReflectivity);
        integral += std::pow(layerReflectivity, 4.0 / 7.0) * layerDepth;
        integratedLayer = true;
    }
    if (!integratedLayer) return std::nullopt;
    const auto vil = 3.44e-6 * integral;
    return std::isfinite(vil) ? std::optional<double>(std::max(0.0, vil)) : std::nullopt;
}

// Column maximum: the strongest reflectivity seen at any tilt over the
// same ground range. No adjacency rule: every sample is a real measurement.
std::optional<double> compositeReflectivityDbz(const std::vector<VerticalSample>& samples) {
    std::optional<double> best;
    for (const auto& sample : samples) {
        if (!best || sample.reflectivityDbz > *best) best = sample.reflectivityDbz;
    }
    return best;
}

// Constant-altitude interpolation at heightMslMeters. Linear in dBZ between
// the adjacent-tilt pair bracketing the height (same no-bridging rule as
// VIL: non-adjacent ordinals never interpolate). Below the lowest sample
// the column reads the lowest value (standard CAPPI clamp); above the
// highest sample there is no data, never an invented top.
std::optional<double> cappiReflectivityDbz(const std::vector<VerticalSample>& samples,
                                           const double heightMslMeters) {
    if (samples.empty() || !std::isfinite(heightMslMeters)) return std::nullopt;
    if (heightMslMeters <= samples.front().heightMslMeters)
        return samples.front().reflectivityDbz;
    for (std::size_t index = 1; index < samples.size(); ++index) {
        const auto& lower = samples[index - 1];
        const auto& upper = samples[index];
        if (upper.tiltOrdinal != lower.tiltOrdinal + 1) continue;
        if (heightMslMeters > upper.heightMslMeters) continue;
        const auto span = upper.heightMslMeters - lower.heightMslMeters;
        if (span <= 0.0) continue;
        const auto fraction =
            std::clamp((heightMslMeters - lower.heightMslMeters) / span, 0.0, 1.0);
        return lower.reflectivityDbz + fraction * (upper.reflectivityDbz - lower.reflectivityDbz);
    }
    return std::nullopt;
}

// REF-convention packing for dBZ-derived fields so palettes, probes, and
// the base moment agree exactly: encoded = round(dbz * 2 + 66), 0 = missing.
std::uint16_t encodeReflectivityDbz(const double dbz) {
    if (!std::isfinite(dbz)) return 0;
    const auto encoded = std::llround(dbz * 2.0 + 66.0);
    if (encoded < 2) return 0;
    return static_cast<std::uint16_t>(std::clamp<long long>(encoded, 2, 65535));
}

} // namespace

// Pure VILD helper (unit-test hook): VIL [kg/m^2] / thickness [m] -> [kg/m^3].
std::optional<double> vilDensityKgPerCubicMeter(const double vilKgPerSquareMeter,
                                                const double layerThicknessMeters) {
    if (!std::isfinite(vilKgPerSquareMeter) || !std::isfinite(layerThicknessMeters))
        return std::nullopt;
    if (vilKgPerSquareMeter < 0.0 || layerThicknessMeters <= 0.0) return std::nullopt;
    const double density = vilKgPerSquareMeter / layerThicknessMeters;
    return std::isfinite(density) ? std::optional<double>(density) : std::nullopt;
}

std::optional<double> vilDensityKgPerCubicMeter(const double vilKgPerSquareMeter,
                                                const double echoTopMeters,
                                                const double baseMeters) {
    if (!std::isfinite(echoTopMeters) || !std::isfinite(baseMeters)) return std::nullopt;
    return vilDensityKgPerCubicMeter(vilKgPerSquareMeter, echoTopMeters - baseMeters);
}

// Cheap pre-check before paying for a deep copy + derive: true when the
// chunk holds reflectivity on at least two distinct elevation numbers.
// Mirrors the <2 tilts early-out inside appendDerivedProducts.
bool chunkSupportsDerivedProducts(const DecodedChunk& chunk) {
    int firstElevation = 0;
    bool haveFirst = false;
    for (const auto& ray : chunk.rays) {
        if (!ray.has(ProductKind::Reflectivity)) continue;
        if (!haveFirst) {
            firstElevation = ray.elevationNumber;
            haveFirst = true;
        } else if (ray.elevationNumber != firstElevation) {
            return true;
        }
    }
    return false;
}

DerivedProductSummary appendDerivedProducts(DecodedChunk& chunk,
                                            const double radarElevationMeters,
                                            const double cappiHeightMslMeters) {
    DerivedProductSummary summary;
    const auto tilts = reflectivityTilts(chunk);
    summary.sourceTiltCount = static_cast<int>(tilts.size());
    if (tilts.size() < 2) return summary;

    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto echoTopIndex = static_cast<std::size_t>(ProductKind::EchoTop);
    const auto vilIndex = static_cast<std::size_t>(ProductKind::VerticallyIntegratedLiquid);
    const auto crefIndex = static_cast<std::size_t>(ProductKind::CompositeReflectivity);
    const auto cappiIndex = static_cast<std::size_t>(ProductKind::Cappi);
    const auto& lowestTilt = tilts.front();
    std::map<LookupKey, std::vector<GateLookup>> lookupCache;

    for (const auto rayIndex : lowestTilt.rayIndexes) {
        auto& outputRay = chunk.rays.at(rayIndex);
        const auto& reflectivity = outputRay.fields.at(reflectivityIndex);
        if (reflectivity.empty()) continue;

        const auto firstGate = outputRay.firstGateByProduct.at(reflectivityIndex);
        const auto spacing = outputRay.gateSpacingByProduct.at(reflectivityIndex);
        if (spacing == 0) continue;
        auto echoTops = std::vector<std::uint16_t>(reflectivity.size(), 0);
        auto vilValues = std::vector<std::uint16_t>(reflectivity.size(), 0);
        auto crefValues = std::vector<std::uint16_t>(reflectivity.size(), 0);
        auto cappiValues = std::vector<std::uint16_t>(reflectivity.size(), 0);
        std::vector<ColumnSource> sources;
        sources.reserve(tilts.size());

        for (std::size_t tiltOrdinal = 0; tiltOrdinal < tilts.size(); ++tiltOrdinal) {
            const auto& tilt = tilts[tiltOrdinal];
            const auto sourceRayIndex = nearestRay(chunk, tilt, outputRay.azimuthDegrees);
            if (!sourceRayIndex) continue;
            const auto& sourceRay = chunk.rays.at(*sourceRayIndex);
            const auto sourceFirstGate = sourceRay.firstGateByProduct.at(reflectivityIndex);
            const auto sourceSpacing = sourceRay.gateSpacingByProduct.at(reflectivityIndex);
            if (sourceSpacing == 0) continue;
            const LookupKey key{
                firstGate, spacing, reflectivity.size(),
                static_cast<int>(std::llround(lowestTilt.meanElevationDegrees * 1000.0)),
                sourceFirstGate, sourceSpacing,
                static_cast<int>(std::llround(tilt.meanElevationDegrees * 1000.0)),
            };
            auto [lookup, inserted] = lookupCache.try_emplace(key);
            if (inserted) {
                lookup->second = buildGateLookup(firstGate, spacing, reflectivity.size(),
                                                 lowestTilt.meanElevationDegrees,
                                                 sourceFirstGate, sourceSpacing,
                                                 tilt.meanElevationDegrees,
                                                 radarElevationMeters);
            }
            sources.push_back({&sourceRay, &lookup->second, tiltOrdinal});
        }

        std::vector<VerticalSample> samples;
        samples.reserve(sources.size());
        for (std::size_t gate = 0; gate < reflectivity.size(); ++gate) {
            samples.clear();
            for (const auto& source : sources) {
                const auto& sample = source.lookup->at(gate);
                const auto& encoded = source.ray->fields.at(reflectivityIndex);
                if (sample.sourceGate < 0 ||
                    static_cast<std::size_t>(sample.sourceGate) >= encoded.size() ||
                    encoded[static_cast<std::size_t>(sample.sourceGate)] <= 1)
                    continue;
                const auto value = nativeValue(
                    encoded[static_cast<std::size_t>(sample.sourceGate)],
                    source.ray->scaleByProduct.at(reflectivityIndex),
                    source.ray->offsetByProduct.at(reflectivityIndex));
                if (!value || !std::isfinite(*value)) continue;
                samples.push_back({sample.heightMslMeters, *value, source.tiltOrdinal});
            }
            if (samples.empty()) continue;

            std::optional<double> echoTopHeightMeters;
            if (const auto echoTop = echoTopMeters(samples)) {
                echoTops[gate] = encodeDerived(echoTop->first / kMetersPerKft);
                if (echoTops[gate] > 1) {
                    ++summary.echoTopGateCount;
                    if (echoTop->second) ++summary.echoTopLowerBoundGateCount;
                    echoTopHeightMeters = echoTop->first;
                }
            }
            std::optional<double> vilValue;
            if (const auto vil = vilKilogramsPerSquareMeter(samples)) {
                vilValues[gate] = encodeDerived(*vil);
                if (vilValues[gate] > 1) {
                    ++summary.vilGateCount;
                    vilValue = *vil;
                }
            }
            if (const auto cref = compositeReflectivityDbz(samples)) {
                crefValues[gate] = encodeReflectivityDbz(*cref);
                if (crefValues[gate] > 1) ++summary.crefGateCount;
            }
            if (const auto cappi = cappiReflectivityDbz(samples, cappiHeightMslMeters)) {
                cappiValues[gate] = encodeReflectivityDbz(*cappi);
                if (cappiValues[gate] > 1) ++summary.cappiGateCount;
            }
            // VILD diagnostic only: no new product field is materialized.
            if (echoTopHeightMeters && vilValue) {
                const double baseHeightMeters = samples.front().heightMslMeters;
                if (const auto density = vilDensityKgPerCubicMeter(
                        *vilValue, *echoTopHeightMeters, baseHeightMeters)) {
                    ++summary.vilDensityGateCount;
                    summary.maxVilDensityKgPerCubicMeter =
                        std::max(summary.maxVilDensityKgPerCubicMeter, *density);
                }
            }
        }

        outputRay.fields.at(echoTopIndex) = std::move(echoTops);
        outputRay.firstGateByProduct.at(echoTopIndex) = firstGate;
        outputRay.gateSpacingByProduct.at(echoTopIndex) = spacing;
        outputRay.scaleByProduct.at(echoTopIndex) = kDerivedScale;
        outputRay.offsetByProduct.at(echoTopIndex) = kDerivedOffset;
        outputRay.fields.at(vilIndex) = std::move(vilValues);
        outputRay.firstGateByProduct.at(vilIndex) = firstGate;
        outputRay.gateSpacingByProduct.at(vilIndex) = spacing;
        outputRay.scaleByProduct.at(vilIndex) = kDerivedScale;
        outputRay.offsetByProduct.at(vilIndex) = kDerivedOffset;
        // dBZ-derived fields share the REF packing so rendering, probing,
        // and palettes agree with the base moment gate for gate.
        outputRay.fields.at(crefIndex) = std::move(crefValues);
        outputRay.firstGateByProduct.at(crefIndex) = firstGate;
        outputRay.gateSpacingByProduct.at(crefIndex) = spacing;
        outputRay.scaleByProduct.at(crefIndex) = 2.0F;
        outputRay.offsetByProduct.at(crefIndex) = 66.0F;
        outputRay.fields.at(cappiIndex) = std::move(cappiValues);
        outputRay.firstGateByProduct.at(cappiIndex) = firstGate;
        outputRay.gateSpacingByProduct.at(cappiIndex) = spacing;
        outputRay.scaleByProduct.at(cappiIndex) = 2.0F;
        outputRay.offsetByProduct.at(cappiIndex) = 66.0F;
        ++summary.outputRayCount;
    }
    return summary;
}

} // namespace cappedweather
