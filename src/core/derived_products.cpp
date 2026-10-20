#include "core/derived_products.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

// ---------------------------------------------------------------------------
// Z-R accumulation internals
// ---------------------------------------------------------------------------

struct RainfallGateMap {
    long long sourceGate = -1;
    double groundRangeMeters = 0.0;
    double beamHeightMeters = 0.0;  // MSL at the source gate centre
};

// Reference gate -> source gate by ground range, with the beam height the
// sample was actually measured at. Same geometry as buildGateLookup, plus the
// range/height provenance the accumulation needs.
std::vector<RainfallGateMap> buildRainfallGateMap(const std::int16_t referenceFirstGate,
                                                  const std::uint16_t referenceSpacing,
                                                  const std::size_t referenceGateCount,
                                                  const double referenceElevationDegrees,
                                                  const std::int16_t sourceFirstGate,
                                                  const std::uint16_t sourceSpacing,
                                                  const std::size_t sourceGateCount,
                                                  const double sourceElevationDegrees,
                                                  const double radarElevationMeters) {
    std::vector<RainfallGateMap> map(referenceGateCount);
    if (referenceSpacing == 0 || sourceSpacing == 0) return map;
    for (std::size_t gate = 0; gate < referenceGateCount; ++gate) {
        const auto referenceSlant = gateCenterSlantRange(referenceFirstGate, referenceSpacing, gate);
        if (referenceSlant <= 0.0) continue;
        const auto groundRange =
            standardBeamGeometry(referenceSlant, referenceElevationDegrees).surfaceDistanceMeters;
        if (!std::isfinite(groundRange) || groundRange <= 0.0) continue;
        const auto sourceSlant = slantRangeForSurfaceDistance(groundRange, sourceElevationDegrees);
        if (!std::isfinite(sourceSlant)) continue;
        const auto position = (sourceSlant - static_cast<double>(sourceFirstGate)) /
                              static_cast<double>(sourceSpacing);
        if (!std::isfinite(position)) continue;
        const auto sourceGate = static_cast<long long>(std::llround(position));
        if (sourceGate < 0 || static_cast<std::size_t>(sourceGate) >= sourceGateCount) continue;
        // Beam height at the gate that is actually read, not at the ideal
        // range: rounding to the nearest source gate moves it a little.
        const auto actualSlant =
            gateCenterSlantRange(sourceFirstGate, sourceSpacing,
                                 static_cast<std::size_t>(sourceGate));
        const auto height = standardBeamGeometry(actualSlant, sourceElevationDegrees)
                                .heightAboveRadarMeters +
                            radarElevationMeters;
        if (!std::isfinite(height)) continue;
        map[gate] = {sourceGate, groundRange, height};
    }
    return map;
}

struct ColumnProfile {
    // Echo top (>= 18 dBZ) in m MSL; NaN where the column carries no echo.
    std::vector<double> echoTopMeters;
    // Highest beam that sampled this gate, echo or not: the ceiling of what the
    // volume could possibly know. NaN where no tilt covered the gate.
    std::vector<double> observedCeilingMeters;
};

// Per-volume column profiles for one lowest-tilt ray. Cached per ray because
// the accumulation asks for the same ray from several reference rays when the
// azimuth grids differ, and the lookup tables are geometry-keyed like the
// product derive.
class ColumnProfiles {
public:
    ColumnProfiles(const DecodedChunk& chunk, std::vector<Tilt> tilts,
                   const double radarElevationMeters)
        : chunk_(chunk), tilts_(std::move(tilts)),
          radarElevationMeters_(radarElevationMeters) {}

    const ColumnProfile& forRay(const std::size_t rayIndex) {
        const auto found = cache_.find(rayIndex);
        if (found != cache_.end()) return found->second;
        return cache_.emplace(rayIndex, compute(rayIndex)).first->second;
    }

private:
    ColumnProfile compute(const std::size_t rayIndex) {
        const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
        const auto& ray = chunk_.rays.at(rayIndex);
        const auto& encoded = ray.fields.at(reflectivityIndex);
        ColumnProfile profile;
        profile.echoTopMeters.assign(encoded.size(), std::numeric_limits<double>::quiet_NaN());
        profile.observedCeilingMeters.assign(encoded.size(),
                                             std::numeric_limits<double>::quiet_NaN());
        const auto firstGate = ray.firstGateByProduct.at(reflectivityIndex);
        const auto spacing = ray.gateSpacingByProduct.at(reflectivityIndex);
        if (spacing == 0 || encoded.empty()) return profile;

        std::vector<ColumnSource> sources;
        sources.reserve(tilts_.size());
        for (std::size_t tiltOrdinal = 0; tiltOrdinal < tilts_.size(); ++tiltOrdinal) {
            const auto& tilt = tilts_[tiltOrdinal];
            const auto sourceRayIndex = nearestRay(chunk_, tilt, ray.azimuthDegrees);
            if (!sourceRayIndex) continue;
            const auto& sourceRay = chunk_.rays.at(*sourceRayIndex);
            const auto sourceFirstGate = sourceRay.firstGateByProduct.at(reflectivityIndex);
            const auto sourceSpacing = sourceRay.gateSpacingByProduct.at(reflectivityIndex);
            if (sourceSpacing == 0) continue;
            const LookupKey key{
                firstGate, spacing, encoded.size(),
                static_cast<int>(std::llround(ray.elevationDegrees * 1000.0)),
                sourceFirstGate, sourceSpacing,
                static_cast<int>(std::llround(tilt.meanElevationDegrees * 1000.0)),
            };
            auto [lookup, inserted] = lookupCache_.try_emplace(key);
            if (inserted) {
                lookup->second = buildGateLookup(firstGate, spacing, encoded.size(),
                                                 ray.elevationDegrees, sourceFirstGate,
                                                 sourceSpacing, tilt.meanElevationDegrees,
                                                 radarElevationMeters_);
            }
            sources.push_back({&sourceRay, &lookup->second, tiltOrdinal});
        }
        if (sources.empty()) return profile;

        std::vector<VerticalSample> samples;
        samples.reserve(sources.size());
        for (std::size_t gate = 0; gate < encoded.size(); ++gate) {
            samples.clear();
            double ceiling = std::numeric_limits<double>::quiet_NaN();
            for (const auto& source : sources) {
                const auto& sample = source.lookup->at(gate);
                if (sample.sourceGate < 0) continue;
                // A tilt with no echo still observed the gate: the ceiling is
                // where observation stops, not where echo stops.
                if (!std::isfinite(ceiling) || sample.heightMslMeters > ceiling)
                    ceiling = sample.heightMslMeters;
                const auto& sourceEncoded = source.ray->fields.at(reflectivityIndex);
                if (static_cast<std::size_t>(sample.sourceGate) >= sourceEncoded.size()) continue;
                if (sourceEncoded[static_cast<std::size_t>(sample.sourceGate)] <= 1) continue;
                const auto value = nativeValue(sourceEncoded[static_cast<std::size_t>(sample.sourceGate)],
                                               source.ray->scaleByProduct.at(reflectivityIndex),
                                               source.ray->offsetByProduct.at(reflectivityIndex));
                if (!value || !std::isfinite(*value)) continue;
                samples.push_back({sample.heightMslMeters, *value, source.tiltOrdinal});
            }
            profile.observedCeilingMeters[gate] = ceiling;
            if (samples.empty()) continue;
            if (const auto top = echoTopMeters(samples)) profile.echoTopMeters[gate] = top->first;
        }
        return profile;
    }

    const DecodedChunk& chunk_;
    std::vector<Tilt> tilts_;
    double radarElevationMeters_ = 0.0;
    std::map<LookupKey, std::vector<GateLookup>> lookupCache_;
    std::map<std::size_t, ColumnProfile> cache_;
};

// Cache key for the reference->source gate maps. The source gate count is part
// of the key: a truncated volume must not reuse a map built for a full one.
struct RainfallMapKey {
    std::int16_t referenceFirstGate = 0;
    std::uint16_t referenceSpacing = 0;
    std::size_t referenceGateCount = 0;
    int referenceElevationMilli = 0;
    std::int16_t sourceFirstGate = 0;
    std::uint16_t sourceSpacing = 0;
    std::size_t sourceGateCount = 0;
    int sourceElevationMilli = 0;

    bool operator<(const RainfallMapKey& other) const {
        return std::make_tuple(referenceFirstGate, referenceSpacing, referenceGateCount,
                               referenceElevationMilli, sourceFirstGate, sourceSpacing,
                               sourceGateCount, sourceElevationMilli) <
               std::make_tuple(other.referenceFirstGate, other.referenceSpacing,
                               other.referenceGateCount, other.referenceElevationMilli,
                               other.sourceFirstGate, other.sourceSpacing,
                               other.sourceGateCount, other.sourceElevationMilli);
    }
};

constexpr double kRainfallPoorCoverageFraction = 0.5;
constexpr double kRainfallPoorContaminationFraction = 0.05;
constexpr int kRainfallMinimumSamplesForGood = 4;
constexpr double kRainfallDepthStorageCapTenths = 65535.0;

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

std::optional<double> zrRateMillimetersPerHour(const double reflectivityDbz, const double zrA,
                                               const double zrB) {
    if (!std::isfinite(reflectivityDbz) || !std::isfinite(zrA) || !std::isfinite(zrB))
        return std::nullopt;
    if (zrA <= 0.0 || zrB <= 0.0) return std::nullopt;
    const auto linearReflectivity = std::pow(10.0, reflectivityDbz / 10.0);
    if (!std::isfinite(linearReflectivity) || linearReflectivity < 0.0) return std::nullopt;
    const auto rate = std::pow(linearReflectivity / zrA, 1.0 / zrB);
    if (!std::isfinite(rate) || rate < 0.0) return std::nullopt;
    return rate;
}

RainfallGrid accumulateRainfall(const std::vector<RainfallSample>& samples,
                                const double radarElevationMeters,
                                const double endSecondsSinceEpoch,
                                const RainfallConfig config) {
    RainfallGrid grid;
    RainfallSummary& summary = grid.summary;

    // Fail closed on a relation or cap that cannot produce an honest number.
    if (!(config.zrA > 0.0) || !(config.zrB > 0.0) ||
        !(config.maximumIntervalSeconds > 0.0) || !std::isfinite(radarElevationMeters))
        return grid;

    // Order by volume time and drop unusable or duplicate entries. Sorting here
    // means a caller that hands in a reversed history still gets an honest
    // integration instead of silently negative intervals.
    std::vector<RainfallSample> usable;
    usable.reserve(samples.size());
    for (const auto& sample : samples) {
        if (!sample.chunk || !std::isfinite(sample.secondsSinceEpoch)) {
            ++summary.rejectedSampleCount;
            continue;
        }
        usable.push_back(sample);
    }
    std::stable_sort(usable.begin(), usable.end(),
                     [](const RainfallSample& left, const RainfallSample& right) {
                         return left.secondsSinceEpoch < right.secondsSinceEpoch;
                     });
    std::vector<RainfallSample> ordered;
    ordered.reserve(usable.size());
    for (const auto& sample : usable) {
        if (!ordered.empty() &&
            !(sample.secondsSinceEpoch - ordered.back().secondsSinceEpoch > 1e-6)) {
            ++summary.rejectedSampleCount;  // duplicate volume time
            continue;
        }
        ordered.push_back(sample);
    }
    summary.sampleCount = static_cast<int>(ordered.size());
    if (!ordered.empty()) grid.radarId = ordered.back().chunk->radarId;
    if (ordered.size() < 2) return grid;  // no interval: Invalid, nothing invented

    summary.windowSeconds =
        ordered.back().secondsSinceEpoch - ordered.front().secondsSinceEpoch;
    if (!(summary.windowSeconds > 0.0)) return grid;

    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto correlationIndex = static_cast<std::size_t>(ProductKind::CorrelationCoefficient);
    const auto& newest = ordered.back();
    const auto referenceTilts = reflectivityTilts(*newest.chunk);
    if (referenceTilts.empty()) return grid;
    const auto& referenceTilt = referenceTilts.front();
    grid.referenceElevationDegrees = referenceTilt.meanElevationDegrees;

    std::vector<std::vector<double>> depths;
    std::vector<std::vector<std::uint8_t>> incomplete;
    depths.reserve(referenceTilt.rayIndexes.size());
    incomplete.reserve(referenceTilt.rayIndexes.size());
    for (const auto rayIndex : referenceTilt.rayIndexes) {
        const auto& ray = newest.chunk->rays.at(rayIndex);
        const auto& field = ray.fields.at(reflectivityIndex);
        if (field.empty()) continue;
        if (grid.rays.empty()) {
            grid.firstGateMeters = ray.firstGateByProduct.at(reflectivityIndex);
            grid.gateSpacingMeters = ray.gateSpacingByProduct.at(reflectivityIndex);
        }
        RainfallRay out;
        out.referenceRayIndex = static_cast<int>(rayIndex);
        out.azimuthDegrees = ray.azimuthDegrees;
        out.gates.assign(field.size(), RainfallGate{});
        grid.rays.push_back(std::move(out));
        depths.emplace_back(field.size(), 0.0);
        incomplete.emplace_back(field.size(), 0);
    }
    if (grid.rays.empty()) return grid;

    // Geometry-keyed mapping cache: the same pair of range tables repeats for
    // every ray and every volume of one product.
    std::map<RainfallMapKey, std::vector<RainfallGateMap>> gateMapCache;

    for (std::size_t sampleIndex = 0; sampleIndex < ordered.size(); ++sampleIndex) {
        double intervalSeconds = 0.0;
        bool gapClamped = false;
        if (sampleIndex + 1 < ordered.size()) {
            intervalSeconds =
                ordered[sampleIndex + 1].secondsSinceEpoch - ordered[sampleIndex].secondsSinceEpoch;
        } else if (std::isfinite(endSecondsSinceEpoch)) {
            intervalSeconds = endSecondsSinceEpoch - ordered[sampleIndex].secondsSinceEpoch;
        }
        if (!(intervalSeconds > 0.0)) continue;
        if (intervalSeconds > config.maximumIntervalSeconds) {
            intervalSeconds = config.maximumIntervalSeconds;
            gapClamped = true;
            ++summary.gapClampedIntervalCount;
        }
        if (!(intervalSeconds > 0.0)) continue;
        ++summary.intervalCount;

        const auto tilts = reflectivityTilts(*ordered[sampleIndex].chunk);
        if (tilts.empty()) {
            // No reflectivity at all for this interval: every gate loses a
            // measurement and the covered time is not credited.
            for (auto& rayFlags : incomplete)
                std::fill(rayFlags.begin(), rayFlags.end(), static_cast<std::uint8_t>(1));
            continue;
        }
        summary.coveredSeconds += intervalSeconds;

        ColumnProfiles profiles(*ordered[sampleIndex].chunk, tilts, radarElevationMeters);
        const auto& lowestTilt = tilts.front();
        const double tenthsPerInterval = intervalSeconds / 360.0;  // rate[mm/h] -> 0.1 mm

        for (std::size_t rayIndex = 0; rayIndex < grid.rays.size(); ++rayIndex) {
            auto& outRay = grid.rays[rayIndex];
            const auto& referenceRay =
                newest.chunk->rays.at(static_cast<std::size_t>(outRay.referenceRayIndex));
            const auto referenceFirstGate = referenceRay.firstGateByProduct.at(reflectivityIndex);
            const auto referenceSpacing = referenceRay.gateSpacingByProduct.at(reflectivityIndex);
            if (referenceSpacing == 0) continue;

            const auto sourceRayIndex =
                nearestRay(*ordered[sampleIndex].chunk, lowestTilt, referenceRay.azimuthDegrees);
            if (!sourceRayIndex) {
                std::fill(incomplete[rayIndex].begin(), incomplete[rayIndex].end(),
                          static_cast<std::uint8_t>(1));
                continue;
            }
            const auto& sourceRay = ordered[sampleIndex].chunk->rays.at(*sourceRayIndex);
            const auto sourceFirstGate = sourceRay.firstGateByProduct.at(reflectivityIndex);
            const auto sourceSpacing = sourceRay.gateSpacingByProduct.at(reflectivityIndex);
            if (sourceSpacing == 0) {
                std::fill(incomplete[rayIndex].begin(), incomplete[rayIndex].end(),
                          static_cast<std::uint8_t>(1));
                continue;
            }
            const auto& sourceEncoded = sourceRay.fields.at(reflectivityIndex);
            const auto& sourceCorrelation = sourceRay.fields.at(correlationIndex);
            // Size the map from this ray's own gates: partial rays in one tilt
            // need not match the first ray's count.
            const auto referenceGateCount = outRay.gates.size();

            const RainfallMapKey key{
                referenceFirstGate, referenceSpacing, referenceGateCount,
                static_cast<int>(std::llround(referenceRay.elevationDegrees * 1000.0)),
                sourceFirstGate, sourceSpacing, sourceEncoded.size(),
                static_cast<int>(std::llround(sourceRay.elevationDegrees * 1000.0)),
            };
            auto [mapEntry, inserted] = gateMapCache.try_emplace(key);
            if (inserted) {
                mapEntry->second = buildRainfallGateMap(
                    referenceFirstGate, referenceSpacing, referenceGateCount,
                    referenceRay.elevationDegrees, sourceFirstGate, sourceSpacing,
                    sourceEncoded.size(), sourceRay.elevationDegrees, radarElevationMeters);
            }
            const auto& gateMap = mapEntry->second;
            const auto& profile = profiles.forRay(*sourceRayIndex);

            for (std::size_t gate = 0; gate < outRay.gates.size(); ++gate) {
                auto& cell = outRay.gates[gate];
                const auto& mapped = gateMap[gate];
                if (mapped.sourceGate < 0) {
                    incomplete[rayIndex][gate] = 1;
                    continue;
                }
                if (mapped.groundRangeMeters > config.maximumRangeMeters) {
                    cell.flags |= RainfallGateRangeLimited;
                    continue;
                }
                if (mapped.beamHeightMeters > config.maximumBeamHeightMeters) {
                    // Too high to be rain: refuse the depth, keep the fact.
                    cell.flags |= RainfallGateBeamHigh;
                    continue;
                }
                const auto sourceGate = static_cast<std::size_t>(mapped.sourceGate);
                if (sourceGate >= sourceEncoded.size()) {
                    incomplete[rayIndex][gate] = 1;
                    continue;
                }
                const auto raw = sourceEncoded[sourceGate];
                // Codes 0 and 1 mean "below threshold" and "missing", and the
                // two decoded formats swap which is which, so the chunk's
                // recorded convention is the only honest source for the
                // difference (docs/moment-sentinels.md). A below-threshold gate
                // was measured and saw no echo: zero depth, no flag. A missing
                // gate is a coverage gap and must say so instead of reading as a
                // dry gate. A chunk that records no convention (legacy spill
                // file) keeps the old, weaker reading: zero depth and no claim
                // either way. None of the three invents rain.
                switch (momentGateState(ordered[sampleIndex].chunk->momentEncoding, raw)) {
                case MomentGateState::Valid:
                    break;
                case MomentGateState::BelowThreshold:
                    continue;
                case MomentGateState::Missing:
                    incomplete[rayIndex][gate] = 1;
                    continue;
                case MomentGateState::Undetermined:
                    continue;  // legacy chunk: cannot claim a gap
                }
                const auto value = nativeValue(raw, sourceRay.scaleByProduct.at(reflectivityIndex),
                                               sourceRay.offsetByProduct.at(reflectivityIndex));
                if (!value || !std::isfinite(*value)) {
                    incomplete[rayIndex][gate] = 1;
                    continue;
                }

                // Dual-pol veto first: CC sees non-meteorological returns that a
                // single-pol threshold cannot.
                if (sourceGate < sourceCorrelation.size()) {
                    const auto ccRaw = sourceCorrelation[sourceGate];
                    if (ccRaw > 1) {
                        const auto cc = nativeValue(ccRaw,
                                                    sourceRay.scaleByProduct.at(correlationIndex),
                                                    sourceRay.offsetByProduct.at(correlationIndex));
                        if (cc && std::isfinite(*cc) &&
                            *cc < config.minimumCorrelationCoefficient) {
                            cell.flags |= RainfallGateNonMeteorological;
                            continue;
                        }
                    }
                }

                double reflectivity = *value;
                if (reflectivity >= config.clutterReflectivityDbz) {
                    // Vertical check: strong echo with a column that stops below
                    // the melt-level floor is clutter or anomalous propagation.
                    // If the volume never looked above that height at this range,
                    // say so instead of pretending the check passed.
                    const auto ceiling = profile.observedCeilingMeters[sourceGate];
                    const auto columnTop = profile.echoTopMeters[sourceGate];
                    if (!std::isfinite(ceiling) || ceiling < config.clutterTopMeters) {
                        cell.flags |= RainfallGateNoVerticalCheck;
                    } else if (std::isfinite(columnTop) && columnTop < config.clutterTopMeters) {
                        cell.flags |= RainfallGateClutterSuspect;
                        continue;
                    }
                }
                if (reflectivity > config.hailCapDbz) {
                    reflectivity = config.hailCapDbz;
                    cell.flags |= RainfallGateHailCapped;
                }
                if (reflectivity < config.minimumReflectivityDbz) continue;
                const auto rate =
                    zrRateMillimetersPerHour(reflectivity, config.zrA, config.zrB);
                if (!rate) continue;
                depths[rayIndex][gate] += *rate * tenthsPerInterval;
                if (mapped.beamHeightMeters > config.beamHeightFlagMeters)
                    cell.flags |= RainfallGateBeamHigh;
                if (gapClamped) cell.flags |= RainfallGateGapClamped;
            }
        }
    }

    // Fold the interval sums into the storage grid and collect the quality
    // evidence in one pass so counts and flags cannot drift apart.
    double depthSum = 0.0;
    for (std::size_t rayIndex = 0; rayIndex < grid.rays.size(); ++rayIndex) {
        auto& outRay = grid.rays[rayIndex];
        for (std::size_t gate = 0; gate < outRay.gates.size(); ++gate) {
            auto& cell = outRay.gates[gate];
            if (incomplete[rayIndex][gate]) cell.flags |= RainfallGateIncomplete;
            const auto tenths = depths[rayIndex][gate];
            if (tenths > 0.0) {
                if (tenths >= kRainfallDepthStorageCapTenths) {
                    cell.depthTenthsMillimeters =
                        static_cast<std::uint16_t>(kRainfallDepthStorageCapTenths);
                    cell.flags |= RainfallGateDepthSaturated;
                } else {
                    cell.depthTenthsMillimeters =
                        static_cast<std::uint16_t>(std::llround(tenths));
                }
            }
            if (cell.depthTenthsMillimeters > 0) {
                ++summary.depthGateCount;
                const auto millimeters = cell.depthTenthsMillimeters / 10.0;
                depthSum += millimeters;
                if (millimeters > summary.maximumMillimeters) {
                    summary.maximumMillimeters = millimeters;
                    summary.maximumRayIndex = outRay.referenceRayIndex;
                    summary.maximumGateIndex = static_cast<int>(gate);
                    summary.haveMaximum = true;
                }
            }
            if (cell.flags & RainfallGateClutterSuspect) ++summary.clutterSuspectGateCount;
            if (cell.flags & RainfallGateNonMeteorological) ++summary.nonMeteorologicalGateCount;
            if (cell.flags & RainfallGateHailCapped) ++summary.hailCappedGateCount;
            if (cell.flags & RainfallGateBeamHigh) ++summary.beamHighGateCount;
            if (cell.flags & RainfallGateRangeLimited) ++summary.rangeLimitedGateCount;
            if (cell.flags & RainfallGateIncomplete) ++summary.incompleteGateCount;
            if (cell.flags & RainfallGateNoVerticalCheck) ++summary.noVerticalCheckGateCount;
            if (cell.flags & RainfallGateGapClamped) ++summary.gapClampedGateCount;
            if (cell.flags & RainfallGateDepthSaturated) ++summary.depthSaturatedGateCount;
        }
    }
    if (summary.depthGateCount > 0)
        summary.meanMillimeters = depthSum / static_cast<double>(summary.depthGateCount);

    if (!(summary.coveredSeconds > 0.0)) return grid;  // still Invalid
    summary.valid = true;

    // Quality is about whether the number may be read as a total, not about how
    // wet the storm was. Per-gate beam/range/hail flags are provenance, counted
    // above; they do not downgrade the whole product on their own.
    const auto contaminated =
        summary.clutterSuspectGateCount + summary.nonMeteorologicalGateCount;
    const auto rainBearing = summary.depthGateCount + contaminated;
    const double contaminationFraction =
        rainBearing > 0 ? static_cast<double>(contaminated) / static_cast<double>(rainBearing)
                        : 0.0;
    if (summary.windowSeconds > 0.0 &&
        summary.coveredSeconds < kRainfallPoorCoverageFraction * summary.windowSeconds) {
        summary.quality = RainfallQuality::Poor;
    } else if (contaminationFraction > kRainfallPoorContaminationFraction) {
        summary.quality = RainfallQuality::Poor;
    } else if (summary.sampleCount < kRainfallMinimumSamplesForGood ||
               summary.gapClampedIntervalCount > 0 || summary.incompleteGateCount > 0 ||
               summary.noVerticalCheckGateCount > 0) {
        summary.quality = RainfallQuality::Marginal;
    } else {
        summary.quality = RainfallQuality::Good;
    }
    return grid;
}

} // namespace cappedweather
