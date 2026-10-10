#include "core/tbss_detector.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cappedweather {

namespace {

constexpr std::size_t kMaxStartGapGates = 2;  // spike starts right behind the core

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
                                      const TbssConfig config) {
    (void)radarElevationMeters;  // detection is slant-range geometry; site fix needs lat/lon only
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

        // Last core gate on this ray (highest slant range with 60+ dBZ).
        std::size_t lastCore = 0;
        bool hasCore = false;
        double maxCore = 0.0;
        for (std::size_t gate = 0; gate < field.size(); ++gate) {
            const auto value = nativeValue(field[gate], scale, offset);
            if (!value || !std::isfinite(*value)) continue;
            if (*value >= config.minCoreDbz) {
                lastCore = gate;
                hasCore = true;
                maxCore = std::max(maxCore, *value);
            }
        }
        if (!hasCore) continue;
        // Spike run: contiguous weak-echo gates starting at most 2 gates
        // behind the core. A nullopt or out-of-window gate ends the run.
        std::size_t runStart = 0;
        bool inRun = false;
        std::size_t runEnd = 0;
        bool terminated = false;
        bool terminatedClean = false;
        for (std::size_t gate = lastCore + 1; gate < field.size(); ++gate) {
            if (gate >= field.size()) break;
            const auto value = nativeValue(field[gate], scale, offset);
            const bool inWindow =
                value && std::isfinite(*value) && *value >= config.spikeMinDbz &&
                *value < config.spikeMaxDbz;
            if (!inRun) {
                // The spike starts right behind the core; weak echo far
                // downrange is another storm, not this core's artifact.
                if (gate > lastCore + 1 + kMaxStartGapGates) break;
                if (inWindow) {
                    inRun = true;
                    runStart = gate;
                } else if (value && std::isfinite(*value) && *value >= config.spikeMaxDbz) {
                    break;  // strong echo right behind the core: another cell, not a spike
                }
                // Echo-free gates before the run are fine (small gap allowed).
                continue;
            }
            if (!inWindow) {
                runEnd = gate - 1;
                terminated = true;
                // Clean end: echo-free air, not a wall of strong echo.
                terminatedClean = !value || !std::isfinite(*value) || *value < config.spikeMinDbz;
                break;
            }
        }
        if (!inRun || !terminated || !terminatedClean) continue;
        const double startSlant = gateCenterSlantRange(firstGate, spacing, runStart);
        const double endSlant = gateCenterSlantRange(firstGate, spacing, runEnd);
        if (startSlant <= 0.0 || endSlant <= startSlant) continue;
        const double lengthKm = (endSlant - startSlant) / 1000.0;
        if (lengthKm < config.minSpikeKm || lengthKm > config.maxSpikeKm) continue;
        const double coreSlant = gateCenterSlantRange(firstGate, spacing, lastCore);
        if (coreSlant <= 0.0) continue;
        const auto beam = standardBeamGeometry(coreSlant, ray.elevationDegrees);
        const auto pos = destinationPoint({radarLatitudeDeg, radarLongitudeDeg},
                                          ray.azimuthDegrees, beam.surfaceDistanceMeters);
        if (!std::isfinite(pos.latitude) || !std::isfinite(pos.longitude)) continue;
        RaySpike spike;
        spike.coreLat = pos.latitude;
        spike.coreLon = pos.longitude;
        spike.coreSlantMeters = coreSlant;
        spike.maxCoreDbz = maxCore;
        spike.spikeStartSlant = startSlant;
        spike.spikeEndSlant = endSlant;
        spike.azimuth = ray.azimuthDegrees;
        spike.elevationNumber = ray.elevationNumber;
        spikes.push_back(spike);
    }

    // Azimuthal continuity per tilt: sort by azimuth, cluster gaps under 2
    // degrees, keep clusters with enough adjacent rays.
    std::sort(spikes.begin(), spikes.end(), [](const RaySpike& a, const RaySpike& b) {
        if (a.elevationNumber != b.elevationNumber) return a.elevationNumber < b.elevationNumber;
        return a.azimuth < b.azimuth;
    });
    std::vector<TbssDetection> detections;
    std::size_t i = 0;
    while (i < spikes.size()) {
        std::size_t j = i + 1;
        while (j < spikes.size() && spikes[j].elevationNumber == spikes[i].elevationNumber &&
               spikes[j].azimuth - spikes[j - 1].azimuth < 2.0)
            ++j;
        if (static_cast<int>(j - i) >= config.minAdjacentRays) {
            const auto* best = &spikes[i];
            double longest = 0.0;
            for (std::size_t k = i; k < j; ++k) {
                if (spikes[k].maxCoreDbz > best->maxCoreDbz) best = &spikes[k];
                longest = std::max(longest, spikes[k].spikeEndSlant - spikes[k].spikeStartSlant);
            }
            TbssDetection detection;
            detection.latitudeDeg = best->coreLat;
            detection.longitudeDeg = best->coreLon;
            detection.maxCoreDbz = best->maxCoreDbz;
            detection.spikeLengthKm = longest / 1000.0;
            detection.rayCount = static_cast<int>(j - i);
            detections.push_back(detection);
        }
        i = j;
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
