#include "core/zdr_column.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace cappedweather {

std::vector<ZdrColumn> detectZdrColumns(const DecodedChunk& chunk, const double radarLatitudeDeg,
                                       const double radarLongitudeDeg,
                                       const double radarElevationMeters,
                                       const ZdrColumnConfig config) {
    const auto zdrIndex = static_cast<std::size_t>(ProductKind::DifferentialReflectivity);
    const auto refIndex = static_cast<std::size_t>(ProductKind::Reflectivity);

    struct Candidate {
        double lat = 0.0;
        double lon = 0.0;
        double heightMsl = 0.0;
        double zdr = 0.0;
        double ref = 0.0;
        int tilt = 0;
    };
    std::vector<Candidate> candidates;

    for (const auto& ray : chunk.rays) {
        if (!ray.has(ProductKind::DifferentialReflectivity) || !ray.has(ProductKind::Reflectivity))
            continue;
        const auto& zdrField = ray.fields.at(zdrIndex);
        const auto& refField = ray.fields.at(refIndex);
        const auto zdrSpacing = ray.gateSpacingByProduct.at(zdrIndex);
        const auto refSpacing = ray.gateSpacingByProduct.at(refIndex);
        if (zdrSpacing == 0 || refSpacing == 0) continue;
        const auto zdrFirst = ray.firstGateByProduct.at(zdrIndex);
        const auto refFirst = ray.firstGateByProduct.at(refIndex);
        for (std::size_t gate = 0; gate < zdrField.size(); ++gate) {
            const auto zdr =
                nativeValue(zdrField[gate], ray.scaleByProduct.at(zdrIndex),
                            ray.offsetByProduct.at(zdrIndex));
            if (!zdr || !std::isfinite(*zdr) || *zdr < config.minZdrDb) continue;
            const double slant = gateCenterSlantRange(zdrFirst, zdrSpacing, gate);
            if (slant <= 0.0) continue;
            // Collocated reflectivity at the same slant range through the
            // REF gate table (moments differ in range tables; never assume
            // aligned indexes).
            const auto refGate =
                gateIndexForSlantRange(slant, refFirst, refSpacing, refField.size());
            if (!refGate) continue;
            const auto ref =
                nativeValue(refField.at(*refGate), ray.scaleByProduct.at(refIndex),
                            ray.offsetByProduct.at(refIndex));
            if (!ref || !std::isfinite(*ref) || *ref < config.minReflectivityDbz) continue;
            const auto beam = standardBeamGeometry(slant, ray.elevationDegrees);
            const auto pos = destinationPoint({radarLatitudeDeg, radarLongitudeDeg},
                                              ray.azimuthDegrees, beam.surfaceDistanceMeters);
            if (!std::isfinite(pos.latitude) || !std::isfinite(pos.longitude)) continue;
            candidates.push_back({pos.latitude, pos.longitude,
                                  beam.heightAboveRadarMeters + radarElevationMeters, *zdr, *ref,
                                  ray.elevationNumber});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.zdr > b.zdr; });

    struct Group {
        std::vector<Candidate> members;
    };
    std::vector<Group> groups;
    for (const auto& cand : candidates) {
        bool placed = false;
        for (auto& group : groups) {
            const auto& seed = group.members.front();
            if (haversineMeters(cand.lat, cand.lon, seed.lat, seed.lon) / 1000.0 <
                config.clusterKm) {
                group.members.push_back(cand);
                placed = true;
                break;
            }
        }
        if (!placed) groups.push_back({{cand}});
    }

    std::vector<ZdrColumn> columns;
    for (const auto& group : groups) {
        double top = -std::numeric_limits<double>::infinity();
        double bottom = std::numeric_limits<double>::infinity();
        double bestZdr = -std::numeric_limits<double>::infinity();
        double bestRef = 0.0;
        double bestLat = 0.0;
        double bestLon = 0.0;
        std::set<int> tilts;
        for (const auto& member : group.members) {
            top = std::max(top, member.heightMsl);
            bottom = std::min(bottom, member.heightMsl);
            tilts.insert(member.tilt);
            if (member.zdr > bestZdr) {
                bestZdr = member.zdr;
                bestRef = member.ref;
                bestLat = member.lat;
                bestLon = member.lon;
            }
        }
        const double depth = top - bottom;
        if (!std::isfinite(depth) || depth < config.minDepthMeters) continue;
        if (top < config.minTopMslMeters) continue;
        if (tilts.size() < 2) continue;
        ZdrColumn column;
        column.latitudeDeg = bestLat;
        column.longitudeDeg = bestLon;
        column.maxZdrDb = bestZdr;
        column.topMslMeters = top;
        column.depthMeters = depth;
        column.maxReflectivityDbz = bestRef;
        column.tiltCount = static_cast<int>(tilts.size());
        columns.push_back(column);
        if (static_cast<int>(columns.size()) >= config.maxColumns) break;
    }
    return columns;
}

}  // namespace cappedweather
