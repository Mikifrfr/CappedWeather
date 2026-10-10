#include "core/tbss_detector.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cappedweather {

namespace {

struct RaySpike {
    double coreLat = 0.0;
    double coreLon = 0.0;
    double coreSlantMeters = 0.0;
    double maxCoreDbz = 0.0;
    double spikeStartSlant = 0.0;
    double spikeEndSlant = 0.0;
    double azimuth = 0.0;
    int elevationNumber = 0;
};

}  // namespace

std::vector<TbssDetection> detectTbss(const DecodedChunk& chunk, const double radarLatitudeDeg,
                                      const double radarLongitudeDeg,
                                      const double radarElevationMeters,
                                      const TbssConfig config, TbssDiagnostics* diagnostics) {
    (void)radarElevationMeters;  // detection is slant-range geometry; site fix needs lat/lon only
    if (diagnostics) *diagnostics = TbssDiagnostics{};
    const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    std::vector<RaySpike> spikes;

    for (const auto& ray : chunk.rays) {
        if (!ray.has(ProductKind::Reflectivity)) continue;
        const auto& field = ray.fields.at(refIndex);
        if (field.empty()) continue;
        const auto spacing = ray.gateSpacingByProduct.at(refIndex);
        if (spacing == 0) continue;
        const auto firstGate = ray.firstGateByProduct.at(refIndex);
        const auto scale = ray.scaleByProduct.at(refIndex);
        const auto offset = ray.offsetByProduct.at(refIndex);

        // Core gates on this ray (60+ dBZ): the uprange association pool.
        std::vector<std::size_t> coreGates;
        for (std::size_t gate = 0; gate < field.size(); ++gate) {
            const auto value = nativeValue(field[gate], scale, offset);
            if (!value || !std::isfinite(*value)) continue;
            if (*value >= config.minCoreDbz) coreGates.push_back(gate);
        }
        if (coreGates.empty()) continue;
        if (diagnostics) ++diagnostics->raysWithCore;
        const auto gateValue = [&](const std::size_t gate) {
            return nativeValue(field[gate], scale, offset);
        };
        // First speckle-tolerant run rooted at a storm edge. States: OUT
        // (seeking a weak gate whose predecessor is storm), IN (counting
        // weak gates, tolerating up to 2 consecutive echo-free gates).
        // A strong gate ends dirty; 3+ consecutive echo-free gates end
        // clean at the last weak gate (fade into clear air); ray end
        // overruns. The run may start far behind the core (storm zone
        // between); association to a 60+ core within the window is below.
        constexpr int kCleanFadeGates = 3;
        bool foundRun = false;
        bool runDirty = false;
        std::size_t runStart = 0, runEnd = 0;
        int weakGates = 0;
        {
            bool inRun = false;
            int gap = 0;
            std::size_t lastWeak = 0;
            for (std::size_t gate = 1; gate < field.size(); ++gate) {
                const auto value = gateValue(gate);
                const bool weak = value && std::isfinite(*value) &&
                                  *value >= config.spikeMinDbz && *value < config.spikeMaxDbz;
                const bool echoFree =
                    !value || !std::isfinite(*value) || *value < config.spikeMinDbz;
                if (!inRun) {
                    if (!weak) continue;
                    const auto before = gateValue(gate - 1);
                    const bool rooted =
                        before && std::isfinite(*before) && *before >= config.rootMinDbz;
                    if (!rooted) continue;
                    inRun = true;
                    runStart = gate;
                    lastWeak = gate;
                    weakGates = 1;
                    gap = 0;
                    continue;
                }
                if (weak) {
                    ++weakGates;
                    lastWeak = gate;
                    gap = 0;
                    continue;
                }
                if (echoFree) {
                    ++gap;
                    if (gap >= kCleanFadeGates) {
                        foundRun = true;
                        runEnd = lastWeak;
                        break;
                    }
                    continue;
                }
                // Strong gate: the run hits another echo mass, not clear air.
                runDirty = true;
                break;
            }
        }
        if (!foundRun) {
            if (diagnostics) {
                if (runDirty) ++diagnostics->dirtyEnd;
                else ++diagnostics->noRun;
            }
            continue;
        }
        if (weakGates < config.minWeakGates) {
            if (diagnostics) ++diagnostics->runTooShort;
            continue;
        }
        // Length gate on the rooted run.
        const double startSlant = gateCenterSlantRange(firstGate, spacing, runStart);
        const double endSlant = gateCenterSlantRange(firstGate, spacing, runEnd);
        if (startSlant <= 0.0 || endSlant <= startSlant) continue;
        const double lengthKm = (endSlant - startSlant) / 1000.0;
        if (lengthKm < config.minSpikeKm || lengthKm > config.maxSpikeKm) {
            if (diagnostics) {
                if (lengthKm < config.minSpikeKm) ++diagnostics->runTooShort;
                else ++diagnostics->runTooLong;
            }
            continue;
        }
        // Core association: strongest 60+ gate within the window uprange.
        double bestCoreDbz = 0.0;
        std::size_t bestCoreGate = 0;
        bool hasLink = false;
        for (const auto coreGate : coreGates) {
            if (coreGate >= runStart) continue;
            const double coreSlant = gateCenterSlantRange(firstGate, spacing, coreGate);
            if (coreSlant <= 0.0 || startSlant - coreSlant > config.maxCoreDistanceKm * 1000.0)
                continue;
            const auto coreValue = gateValue(coreGate);
            if (coreValue && std::isfinite(*coreValue) && *coreValue > bestCoreDbz) {
                bestCoreDbz = *coreValue;
                bestCoreGate = coreGate;
                hasLink = true;
            }
        }
        if (!hasLink) {
            if (diagnostics) ++diagnostics->noRun;
            continue;
        }
        const double coreSlant = gateCenterSlantRange(firstGate, spacing, bestCoreGate);
        if (coreSlant <= 0.0) continue;
        const auto beam = standardBeamGeometry(coreSlant, ray.elevationDegrees);
        const auto pos = destinationPoint({radarLatitudeDeg, radarLongitudeDeg},
                                          ray.azimuthDegrees, beam.surfaceDistanceMeters);
        if (!std::isfinite(pos.latitude) || !std::isfinite(pos.longitude)) continue;
        RaySpike spike;
        spike.coreLat = pos.latitude;
        spike.coreLon = pos.longitude;
        spike.coreSlantMeters = coreSlant;
        spike.maxCoreDbz = bestCoreDbz;
        spike.spikeStartSlant = startSlant;
        spike.spikeEndSlant = endSlant;
        spike.azimuth = ray.azimuthDegrees;
        spike.elevationNumber = ray.elevationNumber;
        spikes.push_back(spike);
    }

    // Cross-tilt proximity clustering: a spike is a 3D feature painting
    // several tilts at slightly different azimuths, so same-tilt chaining
    // strands real spikes as singles. Seeds are strongest-core first;
    // members sit within 5 km of the seed core (storm scale). Narrowness
    // still gates: azimuth spread over 8 degrees or more than 24 rays is a
    // sheet, not a spike.
    constexpr double kClusterKm = 5.0;
    constexpr int kMaxClusterRays = 24;
    std::sort(spikes.begin(), spikes.end(), [](const RaySpike& a, const RaySpike& b) {
        return a.maxCoreDbz > b.maxCoreDbz;
    });
    std::vector<char> claimed(spikes.size(), false);
    std::vector<TbssDetection> detections;
    for (std::size_t s = 0; s < spikes.size(); ++s) {
        if (claimed[s]) continue;
        claimed[s] = true;
        std::vector<std::size_t> members = {s};
        for (std::size_t k = s + 1; k < spikes.size(); ++k) {
            if (claimed[k]) continue;
            const double dist = haversineMeters(spikes[s].coreLat, spikes[s].coreLon,
                                                spikes[k].coreLat, spikes[k].coreLon);
            if (std::isfinite(dist) && dist / 1000.0 <= kClusterKm) {
                claimed[k] = true;
                members.push_back(k);
            }
        }
        if (static_cast<int>(members.size()) < config.minAdjacentRays) {
            if (diagnostics) {
                diagnostics->isolated += static_cast<int>(members.size());
                for (const auto m : members) {
                    if (diagnostics->isolatedPositions.size() >= 32) break;
                    diagnostics->isolatedPositions.emplace_back(spikes[m].coreLat,
                                                                spikes[m].coreLon);
                }
            }
            continue;
        }
        double minAz = spikes[members.front()].azimuth;
        double maxAz = minAz;
        double longest = 0.0;
        for (const auto m : members) {
            minAz = std::min(minAz, spikes[m].azimuth);
            maxAz = std::max(maxAz, spikes[m].azimuth);
            longest = std::max(longest, spikes[m].spikeEndSlant - spikes[m].spikeStartSlant);
        }
        if (maxAz - minAz > config.maxClusterWidthDeg ||
            static_cast<int>(members.size()) > kMaxClusterRays) {
            if (diagnostics) diagnostics->wideCluster += static_cast<int>(members.size());
            continue;
        }
        TbssDetection detection;
        detection.latitudeDeg = spikes[s].coreLat;
        detection.longitudeDeg = spikes[s].coreLon;
        detection.maxCoreDbz = spikes[s].maxCoreDbz;
        detection.spikeLengthKm = longest / 1000.0;
        detection.rayCount = static_cast<int>(members.size());
        detections.push_back(detection);
    }
    std::sort(detections.begin(), detections.end(),
              [](const TbssDetection& a, const TbssDetection& b) {
                  return a.maxCoreDbz > b.maxCoreDbz;
              });
    if (static_cast<int>(detections.size()) > config.maxDetections)
        detections.resize(static_cast<std::size_t>(config.maxDetections));
    return detections;
}

}  // namespace cappedweather
