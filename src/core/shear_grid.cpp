#include "core/shear_grid.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace cappedweather {
namespace {

constexpr double kPi = 3.14159265358979323846;

double signedAzDiffDeg(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d < -180.0) d += 360.0;
    if (d > 180.0) d -= 360.0;
    return d;
}

}  // namespace

std::vector<ShearCell> computeShearGrid(
    const DecodedChunk& chunk, const std::vector<std::vector<float>>& dealiasedVelocity,
    double radarLatitudeDeg, double radarLongitudeDeg, ShearGridConfig config) {
    const auto velocityIndex = static_cast<std::size_t>(ProductKind::Velocity);
    const auto reflIndex = static_cast<std::size_t>(ProductKind::Reflectivity);

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

    std::vector<ShearCell> cells;
    cells.reserve(4096);

    for (const auto& [elevNum, rays] : tilts) {
        if (rays.empty()) continue;
        const float elevDeg = chunk.rays[rays.front()].elevationDegrees;
        // Low-to-mid tilts carry the meso; skip high PPI slices for speed.
        if (elevDeg > 8.0F) continue;
        const int n = static_cast<int>(rays.size());
        for (int ri = 0; ri < n; ++ri) {
            const std::size_t rayIdx = rays[ri];
            const auto& ray = chunk.rays[rayIdx];
            const auto spacing = ray.gateSpacingByProduct[velocityIndex];
            if (spacing == 0) continue;
            const auto firstGate = ray.firstGateByProduct[velocityIndex];
            const auto& vel = dealiasedVelocity[rayIdx];
            if (vel.empty()) continue;
            const bool hasRefl = ray.has(ProductKind::Reflectivity);
            for (std::size_t gate = 0; gate < vel.size(); ++gate) {
                const double slant =
                    static_cast<double>(firstGate) + static_cast<double>(gate) * spacing;
                if (slant <= 0.0) continue;
                const double rangeKm = slant / 1000.0;
                if (rangeKm > config.maxRangeKm) continue;
                // Convective-core guard on the same ray (split-cut Doppler
                // tilts carry both moments; surveillance #1 is never a
                // velocity tilt so it never reaches here).
                if (hasRefl) {
                    const auto gateOpt = gateIndexForSlantRange(
                        slant, ray.firstGateByProduct[reflIndex],
                        ray.gateSpacingByProduct[reflIndex],
                        ray.fields[reflIndex].size());
                    if (!gateOpt) continue;
                    const auto rv = nativeValue(ray.fields[reflIndex][*gateOpt],
                                                ray.scaleByProduct[reflIndex],
                                                ray.offsetByProduct[reflIndex]);
                    if (!rv || *rv < config.minReflectivityDbz) continue;
                }
                // Least-squares fit V = a + shear * x over the window.
                double sumX = 0.0, sumY = 0.0, sumXX = 0.0, sumXY = 0.0;
                int samples = 0;
                for (int dr = -config.azimuthHalfWindow; dr <= config.azimuthHalfWindow; ++dr) {
                    const int rj = ri + dr;
                    if (rj < 0 || rj >= n) continue;
                    const std::size_t nRayIdx = rays[rj];
                    const auto& nRay = chunk.rays[nRayIdx];
                    if (nRayIdx >= dealiasedVelocity.size()) continue;
                    const auto& nVel = dealiasedVelocity[nRayIdx];
                    const double dAzDeg = signedAzDiffDeg(nRay.azimuthDegrees, ray.azimuthDegrees);
                    // Azimuthal distance at this slant (meters, signed).
                    const double xBase = slant * dAzDeg * kPi / 180.0;
                    for (int dg = -config.rangeHalfWindow; dg <= config.rangeHalfWindow; ++dg) {
                        const long g2 = static_cast<long>(gate) + dg;
                        if (g2 < 0 || g2 >= static_cast<long>(nVel.size())) continue;
                        const float y = nVel[static_cast<std::size_t>(g2)];
                        if (!std::isfinite(y)) continue;
                        // Range offset is second-order for azimuthal shear;
                        // keep x purely azimuthal so the fit is dV/dx.
                        const double x = xBase;
                        sumX += x;
                        sumY += y;
                        sumXX += x * x;
                        sumXY += x * y;
                        ++samples;
                    }
                }
                if (samples < config.minSamples) continue;
                const double meanX = sumX / samples;
                const double meanY = sumY / samples;
                const double varX = sumXX - sumX * meanX;
                if (varX <= 0.0) continue;
                const double shear = (sumXY - sumX * meanY) / varX;
                if (std::abs(shear) < config.minShearPerSecond) continue;
                // Residual rejects folded windows (a fold blows RMSE).
                double sse = 0.0;
                int rcount = 0;
                for (int dr = -config.azimuthHalfWindow; dr <= config.azimuthHalfWindow; ++dr) {
                    const int rj = ri + dr;
                    if (rj < 0 || rj >= n) continue;
                    const std::size_t nRayIdx = rays[rj];
                    const auto& nRay = chunk.rays[nRayIdx];
                    const auto& nVel = dealiasedVelocity[nRayIdx];
                    const double dAzDeg = signedAzDiffDeg(nRay.azimuthDegrees, ray.azimuthDegrees);
                    const double xBase = slant * dAzDeg * kPi / 180.0;
                    for (int dg = -config.rangeHalfWindow; dg <= config.rangeHalfWindow; ++dg) {
                        const long g2 = static_cast<long>(gate) + dg;
                        if (g2 < 0 || g2 >= static_cast<long>(nVel.size())) continue;
                        const float y = nVel[static_cast<std::size_t>(g2)];
                        if (!std::isfinite(y)) continue;
                        const double pred = meanY + shear * (xBase - meanX);
                        const double r = static_cast<double>(y) - pred;
                        sse += r * r;
                        ++rcount;
                    }
                }
                const double rmse = rcount > 0 ? std::sqrt(sse / rcount) : 1e9;
                if (rmse > config.maxResidualMetersPerSecond) continue;

                const auto beam = standardBeamGeometry(std::max(0.0, slant),
                                                       ray.elevationDegrees);
                const auto pos = destinationPoint({radarLatitudeDeg, radarLongitudeDeg},
                                                  ray.azimuthDegrees,
                                                  beam.surfaceDistanceMeters);
                ShearCell cell;
                cell.latitudeDeg = pos.latitude;
                cell.longitudeDeg = pos.longitude;
                cell.heightMetersAgl = beam.heightAboveRadarMeters;
                cell.rangeKm = rangeKm;
                cell.shearPerSecond = shear;
                cell.residualMps = rmse;
                cell.samples = samples;
                cell.elevationDeg = ray.elevationDegrees;
                cell.elevationNumber = ray.elevationNumber;
                cells.push_back(cell);
            }
        }
    }
    return cells;
}

std::vector<ShearPeak> detectShearPeaks(
    const DecodedChunk& chunk, const std::vector<std::vector<float>>& dealiasedVelocity,
    double radarLatitudeDeg, double radarLongitudeDeg, double radarElevationMeters,
    ShearGridConfig config) {
    auto cells = computeShearGrid(chunk, dealiasedVelocity, radarLatitudeDeg,
                                  radarLongitudeDeg, config);
    if (cells.empty()) return {};
    // Greedy local maxima PER TILT: a global pass would collapse the two tilts
    // (same lat/lon) into one peak and break vertical association. Group first.
    std::map<std::uint8_t, std::vector<ShearCell>> cellsByTilt;
    for (const auto& c : cells) cellsByTilt[c.elevationNumber].push_back(c);
    constexpr double kMinPeakSepKm = 4.0;
    std::map<std::uint8_t, std::vector<ShearCell>> peaksByTilt;
    for (auto& [elev, group] : cellsByTilt) {
        std::sort(group.begin(), group.end(), [](const ShearCell& a, const ShearCell& b) {
            return std::abs(a.shearPerSecond) > std::abs(b.shearPerSecond);
        });
        std::vector<ShearCell> peaks;
        for (const auto& c : group) {
            bool tooClose = false;
            for (const auto& p : peaks) {
                if (haversineMeters(c.latitudeDeg, c.longitudeDeg, p.latitudeDeg,
                                    p.longitudeDeg) /
                        1000.0 <
                    kMinPeakSepKm) {
                    tooClose = true;
                    break;
                }
            }
            if (!tooClose) peaks.push_back(c);
            if (peaks.size() >= 100) break;
        }
        peaksByTilt[elev] = std::move(peaks);
    }
    // Vertical association across elevation numbers (same greedy columns as
    // the couplet detector).
    struct Column {
        std::vector<ShearCell> members;
    };
    std::vector<Column> columns;
    for (auto& [elev, group] : peaksByTilt) {
        std::vector<char> used(group.size(), false);
        for (auto& column : columns) {
            const auto& top = column.members.back();
            double best = -1.0;
            double bestDist = 0.0;
            for (std::size_t i = 0; i < group.size(); ++i) {
                if (used[i]) continue;
                const double dist = haversineMeters(top.latitudeDeg, top.longitudeDeg,
                                                    group[i].latitudeDeg, group[i].longitudeDeg);
                if (dist / 1000.0 <= config.verticalAlignKm &&
                    (best < 0.0 || dist < bestDist)) {
                    best = static_cast<double>(i);
                    bestDist = dist;
                }
            }
            if (best >= 0.0) {
                column.members.push_back(group[static_cast<std::size_t>(best)]);
                used[static_cast<std::size_t>(best)] = true;
            }
        }
        for (std::size_t i = 0; i < group.size(); ++i) {
            if (!used[i]) columns.push_back(Column{{group[i]}});
        }
    }
    std::vector<ShearPeak> out;
    for (const auto& column : columns) {
        if (static_cast<int>(column.members.size()) < config.minTilts) continue;
        const ShearCell* best = &column.members.front();
        double minH = best->heightMetersAgl, maxH = best->heightMetersAgl;
        for (const auto& m : column.members) {
            if (std::abs(m.shearPerSecond) > std::abs(best->shearPerSecond)) best = &m;
            minH = std::min(minH, m.heightMetersAgl);
            maxH = std::max(maxH, m.heightMetersAgl);
        }
        ShearPeak peak;
        peak.latitudeDeg = best->latitudeDeg;
        peak.longitudeDeg = best->longitudeDeg;
        peak.shearPerSecond = best->shearPerSecond;
        peak.equivalentDeltaVMps = std::abs(best->shearPerSecond) * 3000.0;
        peak.diameterKm = 3.0;
        peak.depthMeters = maxH - minH;
        peak.tiltCount = static_cast<int>(column.members.size());
        out.push_back(peak);
    }
    std::sort(out.begin(), out.end(), [](const ShearPeak& a, const ShearPeak& b) {
        return std::abs(a.shearPerSecond) > std::abs(b.shearPerSecond);
    });
    if (static_cast<int>(out.size()) > config.maxDetections)
        out.resize(static_cast<std::size_t>(config.maxDetections));
    return out;
}

}  // namespace cappedweather
