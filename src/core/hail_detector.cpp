#include "core/hail_detector.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace cappedweather {

int hailTier(const double vildGPerCubicMeter) {
    if (!std::isfinite(vildGPerCubicMeter)) return 0;
    if (vildGPerCubicMeter >= 4.5) return 3;
    if (vildGPerCubicMeter >= 4.0) return 2;
    if (vildGPerCubicMeter >= 3.5) return 1;
    return 0;
}

std::string_view hailSizeHint(const int tier) {
    switch (tier) {
        case 3: return "golfball or larger possible";
        case 2: return "nickel to golfball";
        case 1: return "dime to nickel";
        default: return "below severe guidance";
    }
}

std::vector<HailCore> detectHailCores(const DecodedChunk& chunk,
                                      const double radarLatitudeDeg,
                                      const double radarLongitudeDeg,
                                      const double radarElevationMeters,
                                      const HailConfig config) {
    (void)radarElevationMeters;  // ET18 arrives as MSL; param kept for API symmetry
    const auto reflIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto vilIndex = static_cast<std::size_t>(ProductKind::VerticallyIntegratedLiquid);
    const auto etIndex = static_cast<std::size_t>(ProductKind::EchoTop);

    struct Candidate {
        double lat = 0.0;
        double lon = 0.0;
        double vild = 0.0;
        double vil = 0.0;
        double topM = 0.0;
        double ref = 0.0;
    };
    std::vector<Candidate> candidates;

    for (const auto& ray : chunk.rays) {
        const auto& vilField = ray.fields.at(vilIndex);
        const auto& etField = ray.fields.at(etIndex);
        const auto& reflField = ray.fields.at(reflIndex);
        if (vilField.empty() || etField.empty() || reflField.empty()) continue;
        const auto spacing = ray.gateSpacingByProduct.at(reflIndex);
        if (spacing == 0) continue;
        const auto firstGate = ray.firstGateByProduct.at(reflIndex);
        const auto gates = std::min({vilField.size(), etField.size(), reflField.size()});
        for (std::size_t gate = 0; gate < gates; ++gate) {
            const auto vil = nativeValue(vilField[gate], ray.scaleByProduct.at(vilIndex),
                                         ray.offsetByProduct.at(vilIndex));
            const auto etKft = nativeValue(etField[gate], ray.scaleByProduct.at(etIndex),
                                           ray.offsetByProduct.at(etIndex));
            if (!vil || !etKft) continue;
            if (*vil < config.minVilKgPerSquareMeter) continue;
            const double topMeters = *etKft * 304.8;
            if (topMeters < config.minEchoTopMeters) continue;
            const double vild = *vil * 1000.0 / topMeters;
            if (vild < config.minVildGPerCubicMeter) continue;
            const auto ref = nativeValue(reflField[gate], ray.scaleByProduct.at(reflIndex),
                                         ray.offsetByProduct.at(reflIndex));
            if (!ref || *ref < config.minReflectivityDbz) continue;
            const double slant = gateCenterSlantRange(firstGate, spacing, gate);
            if (slant <= 0.0) continue;
            const auto beam = standardBeamGeometry(slant, ray.elevationDegrees);
            const auto pos = destinationPoint({radarLatitudeDeg, radarLongitudeDeg},
                                              ray.azimuthDegrees, beam.surfaceDistanceMeters);
            candidates.push_back({pos.latitude, pos.longitude, vild, *vil, topMeters, *ref});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.vild > b.vild; });

    std::vector<HailCore> cores;
    for (const auto& cand : candidates) {
        bool crowded = false;
        for (const auto& kept : cores) {
            if (haversineMeters(cand.lat, cand.lon, kept.latitudeDeg, kept.longitudeDeg) / 1000.0 <
                config.clusterKm) {
                crowded = true;
                break;
            }
        }
        if (crowded) continue;
        HailCore core;
        core.latitudeDeg = cand.lat;
        core.longitudeDeg = cand.lon;
        core.maxVildGPerCubicMeter = cand.vild;
        core.maxVilKgPerSquareMeter = cand.vil;
        core.echoTopMslMeters = cand.topM;  // ET18 field is already MSL
        core.maxReflectivityDbz = cand.ref;
        core.tier = hailTier(cand.vild);
        cores.push_back(core);
        if (static_cast<int>(cores.size()) >= config.maxCores) break;
    }
    return cores;
}

}  // namespace cappedweather
