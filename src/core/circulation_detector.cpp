#include "core/circulation_detector.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>

namespace cappedweather {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Couplet {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double heightMetersAgl = 0.0;
    double rangeKm = 0.0;
    double deltaV = 0.0;
    double diameterKm = 0.0;
    double azimuthMidDeg = 0.0;
};

double angularSepDeg(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d < -180.0) d += 360.0;
    if (d > 180.0) d -= 360.0;
    return std::abs(d);
}

// Collocated reflectivity at a velocity-gate slant range. Fails closed
// (nullopt) when the ray carries no reflectivity moment or the gate is
// missing/folded, so couplets in echo-free air can never qualify.
std::optional<double> reflectivityAt(const DecodedRay& rayA, const DecodedRay& rayB,
                                     const double slantMeters) {
    const auto index = static_cast<std::size_t>(ProductKind::Reflectivity);
    double best = -std::numeric_limits<double>::infinity();
    bool found = false;
    for (const auto* ray : {&rayA, &rayB}) {
        const auto& gates = ray->fields[index];
        if (gates.empty()) continue;
        const auto gate = gateIndexForSlantRange(slantMeters, ray->firstGateByProduct[index],
                                                 ray->gateSpacingByProduct[index], gates.size());
        if (!gate) continue;
        const auto value =
            nativeValue(gates[*gate], ray->scaleByProduct[index], ray->offsetByProduct[index]);
        if (value) {
            found = true;
            best = std::max(best, *value);
        }
    }
    if (!found) return std::nullopt;
    return best;
}

}  // namespace

std::vector<CirculationDetection> detectCirculations(
    const DecodedChunk& chunk, const std::vector<std::vector<float>>& dealiasedVelocity,
    const double radarLatitudeDeg, const double radarLongitudeDeg, const double radarElevationMeters,
    const CirculationConfig config) {
    const auto velocityIndex = static_cast<std::size_t>(ProductKind::Velocity);
    // Group ray indices by elevation number, each sorted by azimuth.
    std::map<std::uint8_t, std::vector<std::size_t>> tilts;
    for (std::size_t i = 0; i < chunk.rays.size(); ++i) {
        const auto& ray = chunk.rays[i];
        if (!ray.has(ProductKind::Velocity)) continue;
        if (i >= dealiasedVelocity.size()) continue;
        tilts[ray.elevationNumber].push_back(i);
    }
    for (auto& [elev, rays] : tilts) {
        std::sort(rays.begin(), rays.end(), [&chunk](std::size_t a, std::size_t b) {
            return chunk.rays[a].azimuthDegrees < chunk.rays[b].azimuthDegrees;
        });
    }

    struct TiltCouplets {
        double elevationDeg = 0.0;
        std::vector<Couplet> couplets;
    };
    std::vector<TiltCouplets> perTilt;
    perTilt.reserve(tilts.size());

    for (const auto& [elev, rays] : tilts) {
        if (rays.size() < 2) continue;
        TiltCouplets tilt;
        tilt.elevationDeg = chunk.rays[rays.front()].elevationDegrees;
        const std::size_t pairs = rays.size();  // includes wraparound pair
        for (std::size_t p = 0; p < pairs; ++p) {
            const auto& rayA = chunk.rays[rays[p]];
            const auto& rayB = chunk.rays[rays[(p + 1) % rays.size()]];
            if (p == rays.size() - 1) {
                // Wraparound pair only when the sweep actually wraps (gap < 5 deg).
                if (angularSepDeg(rayB.azimuthDegrees, rayA.azimuthDegrees) > 5.0) continue;
            }
            const auto& velA = dealiasedVelocity[rays[p]];
            const auto& velB = dealiasedVelocity[rays[(p + 1) % rays.size()]];
            const auto spacing = rayA.gateSpacingByProduct[velocityIndex];
            if (spacing == 0) continue;
            const auto firstGate = rayA.firstGateByProduct[velocityIndex];
            const auto sepDeg = angularSepDeg(rayB.azimuthDegrees, rayA.azimuthDegrees);
            if (sepDeg <= 0.0 || sepDeg > 5.0) continue;
            const auto gateCount = std::min(velA.size(), velB.size());
            // Range-adjacent couplet gates cluster into one candidate.
            // Single-gate hits are speckle (receiver noise is gate-scale);
            // real couplets span several 250 m gates.
            bool inRun = false;
            Couplet run;
            double runBestDeltaV = 0.0;
            double runBestSlant = 0.0;
            int runGates = 0;
            auto flushRun = [&]() {
                if (inRun && runGates >= config.minRunGates &&
                    runBestDeltaV >= config.mesoBaseDeltaV + config.mesoRangeSlope * run.rangeKm &&
                    reflectivityAt(chunk.rays[rays[p]], chunk.rays[rays[(p + 1) % rays.size()]],
                                   runBestSlant)
                            .value_or(-100.0) >= config.minReflectivityDbz) {
                    tilt.couplets.push_back(run);
                }
                inRun = false;
                runBestDeltaV = 0.0;
                runGates = 0;
            };
            int gap = 0;
            for (std::size_t gate = 0; gate < gateCount; ++gate) {
                const double v0 = velA[gate];
                const double v1 = velB[gate];
                const bool couplet =
                    std::isfinite(v0) && std::isfinite(v1) &&
                    ((v0 < -config.minGateVelocityMetersPerSecond &&
                      v1 > config.minGateVelocityMetersPerSecond) ||
                     (v0 > config.minGateVelocityMetersPerSecond &&
                      v1 < -config.minGateVelocityMetersPerSecond));
                const double slant = static_cast<double>(firstGate) +
                                     static_cast<double>(gate) * spacing;
                if (!couplet) {
                    if (inRun && ++gap > config.maxGateGap) flushRun();
                    continue;
                }
                gap = 0;
                const double deltaV = std::abs(v1 - v0);
                const auto beam = standardBeamGeometry(std::max(0.0, slant),
                                                       rayA.elevationDegrees);
                const double azMid = rayA.azimuthDegrees + (rayB.azimuthDegrees > rayA.azimuthDegrees
                                                                ? sepDeg * 0.5
                                                                : -sepDeg * 0.5);
                const auto pos =
                    destinationPoint({radarLatitudeDeg, radarLongitudeDeg}, azMid,
                                     beam.surfaceDistanceMeters);
                const double rangeKm = slant / 1000.0;
                const double diameter = sepDeg * kPi / 180.0 * slant / 1000.0;
                if (!inRun) {
                    inRun = true;
                    run = Couplet{};
                }
                runGates += 1;
                if (deltaV > runBestDeltaV) {
                    runBestDeltaV = deltaV;
                    runBestSlant = slant;
                    runBestDeltaV = deltaV;
                    run.latitudeDeg = pos.latitude;
                    run.longitudeDeg = pos.longitude;
                    run.heightMetersAgl = beam.heightAboveRadarMeters;
                    run.rangeKm = rangeKm;
                    run.deltaV = deltaV;
                    run.diameterKm = diameter;
                    run.azimuthMidDeg = azMid;
                }
            }
            flushRun();
        }
        if (!tilt.couplets.empty()) perTilt.push_back(std::move(tilt));
    }

    // Vertical association: greedy low-to-high columns within align tolerance.
    struct Column {
        std::vector<Couplet> members;
    };
    std::vector<Column> columns;
    for (auto& tilt : perTilt) {
        std::vector<char> used(tilt.couplets.size(), false);
        // Extend existing columns from the column top (one skipped tilt
        // tolerated); match the nearest couplet within align tolerance.
        for (auto& column : columns) {
            const auto& top = column.members.back();
            double best = -1.0;
            double bestDist = 0.0;
            for (std::size_t i = 0; i < tilt.couplets.size(); ++i) {
                if (used[i]) continue;
                const auto& cand = tilt.couplets[i];
                const double dist = haversineMeters(top.latitudeDeg, top.longitudeDeg,
                                                    cand.latitudeDeg, cand.longitudeDeg);
                if (dist / 1000.0 <= config.verticalAlignKm &&
                    (best < 0.0 || dist < bestDist)) {
                    best = static_cast<double>(i);
                    bestDist = dist;
                }
            }
            if (best >= 0.0) {
                column.members.push_back(tilt.couplets[static_cast<std::size_t>(best)]);
                used[static_cast<std::size_t>(best)] = true;
            }
        }
        for (std::size_t i = 0; i < tilt.couplets.size(); ++i) {
            if (!used[i]) columns.push_back(Column{{tilt.couplets[i]}});
        }
    }

    std::vector<CirculationDetection> detections;
    for (const auto& column : columns) {
        if (static_cast<int>(column.members.size()) < config.minTilts) continue;
        const auto* best = &column.members.front();
        double minH = column.members.front().heightMetersAgl;
        double maxH = minH;
        for (const auto& m : column.members) {
            if (m.deltaV > best->deltaV) best = &m;
            minH = std::min(minH, m.heightMetersAgl);
            maxH = std::max(maxH, m.heightMetersAgl);
        }
        const double depth = maxH - minH;
        const double rangeKm = best->rangeKm;
        const double mesoThresh = config.mesoBaseDeltaV + config.mesoRangeSlope * rangeKm;
        const double tvsThresh = config.tvsBaseDeltaV + config.tvsRangeSlope * rangeKm;
        CirculationDetection det;
        det.latitudeDeg = best->latitudeDeg;
        det.longitudeDeg = best->longitudeDeg;
        det.maxDeltaVelocityMetersPerSecond = best->deltaV;
        det.diameterKm = best->diameterKm;
        det.depthMeters = depth;
        det.baseMetersMsl = radarElevationMeters + minH;
        det.topMetersMsl = radarElevationMeters + maxH;
        det.tiltCount = static_cast<int>(column.members.size());
        if (best->deltaV >= tvsThresh && best->diameterKm <= config.tvsMaxDiameterKm &&
            depth >= config.minDepthTvsMeters) {
            det.kind = CirculationKind::TornadoVortex;
        } else if (best->deltaV >= mesoThresh && depth >= config.minDepthMesoMeters) {
            det.kind = CirculationKind::Mesocyclone;
        } else {
            continue;  // shallow/weak shear is already covered by the shear overlay
        }
        detections.push_back(det);
    }
    std::sort(detections.begin(), detections.end(), [](const auto& a, const auto& b) {
        return a.maxDeltaVelocityMetersPerSecond > b.maxDeltaVelocityMetersPerSecond;
    });
    if (static_cast<int>(detections.size()) > config.maxDetections)
        detections.resize(static_cast<std::size_t>(config.maxDetections));
    return detections;
}

}  // namespace cappedweather
