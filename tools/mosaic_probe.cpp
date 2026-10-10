// Mosaic seam probe (offline prototype): two synced single-radar volumes,
// one geographic grid, matched-tilt reflectivity from each, and the seam
// numbers a mosaic commit needs: coverage gain, both-echo disagreement
// (bias/RMSE/|d|>5 dB share), and beam-height mismatch where both see echo.
// Same decode + geometry as the live canvas (decodeLevel2Archive,
// rangeAndBearing, gateIndexForSlantRange, nativeValue), so the numbers
// describe production sampling, not a toy model. Deterministic: fixed grid,
// no network, exit 0 with a report, nonzero on misuse or decode failure.

#include "app/archive_level2.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "core/radar_catalog.hpp"
#include "core/radar_geometry.hpp"

#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {

using cappedweather::BeamGeometry;

struct VolumeView {
    std::string radarId;
    double siteLat = 0.0;
    double siteLon = 0.0;
    double siteMslMeters = 0.0;
    cappedweather::DecodedChunk chunk;
    double tiltElevationDegrees = 0.0;
    // Azimuth-sorted (azimuth, ray index) over the matched tilt only.
    std::vector<std::pair<double, std::size_t>> rayIndex;
};

struct CellSample {
    bool hasEcho = false;
    double dbz = std::numeric_limits<double>::quiet_NaN();
    double beamMslMeters = std::numeric_limits<double>::quiet_NaN();
};

bool loadVolume(const std::string& path, const std::string& radarId, double targetElev,
                VolumeView& out, std::string& error) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        error = "cannot open " + path;
        return false;
    }
    QString qerror;
    auto decoded = cappedweather::decodeLevel2Archive(file.readAll(), &qerror,
                                                      QString::fromStdString(radarId));
    if (!decoded) {
        error = "decode failed for " + path + ": " + qerror.toStdString();
        return false;
    }
    const auto& chunk = decoded->decoded;
    // Site from the catalog: coordinates plus antenna MSL for beam heights.
    bool foundSite = false;
    for (const auto& site : cappedweather::RadarCatalog::defaultSites()) {
        if (site.id == decoded->radarId && site.latitude && site.longitude) {
            out.siteLat = *site.latitude;
            out.siteLon = *site.longitude;
            out.siteMslMeters = site.antennaElevationMeters;
            foundSite = true;
            break;
        }
    }
    if (!foundSite) {
        error = "site " + decoded->radarId.toStdString() + " not in catalog";
        return false;
    }
    out.radarId = decoded->radarId.toStdString();
    // Matched tilt: elevation closest to target among reflectivity rays.
    double best = std::numeric_limits<double>::infinity();
    for (const auto& ray : chunk.rays) {
        if (!ray.has(cappedweather::ProductKind::Reflectivity)) continue;
        best = std::min(best, std::abs(ray.elevationDegrees - targetElev));
    }
    if (!std::isfinite(best)) {
        error = "no reflectivity in " + path;
        return false;
    }
    double tiltElev = 0.0;
    bool haveTilt = false;
    for (const auto& ray : chunk.rays) {
        if (!ray.has(cappedweather::ProductKind::Reflectivity)) continue;
        if (std::abs(ray.elevationDegrees - targetElev) <= best + 0.05 &&
            (!haveTilt || ray.elevationDegrees < tiltElev)) {
            tiltElev = ray.elevationDegrees;
            haveTilt = true;
        }
    }
    out.tiltElevationDegrees = tiltElev;
    out.chunk = chunk;
    for (std::size_t i = 0; i < out.chunk.rays.size(); ++i) {
        const auto& ray = out.chunk.rays[i];
        if (!ray.has(cappedweather::ProductKind::Reflectivity)) continue;
        if (std::abs(ray.elevationDegrees - tiltElev) > 0.05) continue;
        out.rayIndex.emplace_back(ray.azimuthDegrees, i);
    }
    if (out.rayIndex.empty()) {
        error = "matched tilt is empty in " + path;
        return false;
    }
    std::sort(out.rayIndex.begin(), out.rayIndex.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return true;
}

CellSample sampleVolume(const VolumeView& view, double lat, double lon, double echoThresholdDbz) {
    using namespace cappedweather;
    CellSample sample;
    const auto location =
        rangeAndBearing({view.siteLat, view.siteLon}, {lat, lon});
    if (!std::isfinite(location.distanceMeters) || location.distanceMeters < 1000.0 ||
        location.distanceMeters > 460000.0)
        return sample;
    const auto circularDifference = [azimuth = location.bearingDegrees](double candidate) {
        const double direct = std::abs(candidate - azimuth);
        return std::min(direct, 360.0 - direct);
    };
    const auto candidate =
        std::lower_bound(view.rayIndex.cbegin(), view.rayIndex.cend(), location.bearingDegrees,
                         [](const auto& indexed, double target) { return indexed.first < target; });
    auto nearest = candidate == view.rayIndex.cend() ? view.rayIndex.cbegin() : candidate;
    auto nearestDiff = circularDifference(nearest->first);
    const auto previous = candidate == view.rayIndex.cbegin()
                              ? std::prev(view.rayIndex.cend())
                              : std::prev(candidate);
    if (circularDifference(previous->first) < nearestDiff) {
        nearest = previous;
        nearestDiff = circularDifference(previous->first);
    }
    constexpr std::size_t kProduct = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto& ray = view.chunk.rays.at(nearest->second);
    if (nearestDiff > std::max(1.5, ray.azimuthResolutionDegrees * 1.5)) return sample;
    const auto slant =
        slantRangeForSurfaceDistance(location.distanceMeters, ray.elevationDegrees);
    const auto gate = gateIndexForSlantRange(slant, ray.firstGateByProduct.at(kProduct),
                                            ray.gateSpacingByProduct.at(kProduct),
                                            ray.fields.at(kProduct).size());
    if (!gate) return sample;
    const auto value = nativeValue(ray.fields.at(kProduct).at(*gate),
                                   ray.scaleByProduct.at(kProduct), ray.offsetByProduct.at(kProduct));
    if (!value.has_value()) return sample;
    const BeamGeometry beam = standardBeamGeometry(slant, ray.elevationDegrees);
    sample.beamMslMeters = view.siteMslMeters + beam.heightAboveRadarMeters;
    sample.dbz = *value;
    sample.hasEcho = *value >= echoThresholdDbz;
    return sample;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::string pathA, pathB, idA, idB;
    double targetElev = 0.5, boxHalfDeg = 2.0, stepDeg = 0.02, threshDbz = 10.0;
    double centerLat = std::numeric_limits<double>::quiet_NaN();
    double centerLon = std::numeric_limits<double>::quiet_NaN();
    for (int i = 1; i < argc;) {
        const std::string flag = argv[i];
        const auto need = [&](std::string& out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "mosaic_probe: %s needs a value\n", flag.c_str());
                return false;
            }
            out = argv[i + 1];
            i += 2;
            return true;
        };
        const auto needDouble = [&](double& out) {
            std::string text;
            if (!need(text)) return false;
            char* end = nullptr;
            out = std::strtod(text.c_str(), &end);
            if (end == text.c_str() || !std::isfinite(out)) {
                std::fprintf(stderr, "mosaic_probe: bad number for %s\n", flag.c_str());
                return false;
            }
            return true;
        };
        if (flag == "--a" || flag == "--b" || flag == "--ida" || flag == "--idb") {
            std::string* out = flag == "--a" ? &pathA
                               : flag == "--b" ? &pathB
                               : flag == "--ida" ? &idA
                                                 : &idB;
            if (!need(*out)) return EXIT_FAILURE;
            continue;
        }
        if (flag == "--elev" || flag == "--box" || flag == "--step" || flag == "--thresh" ||
            flag == "--clat" || flag == "--clon") {
            double* out = flag == "--elev"    ? &targetElev
                          : flag == "--box"   ? &boxHalfDeg
                          : flag == "--step"  ? &stepDeg
                          : flag == "--thresh" ? &threshDbz
                          : flag == "--clat"   ? &centerLat
                                               : &centerLon;
            if (!needDouble(*out)) return EXIT_FAILURE;
            continue;
        }
        std::fprintf(stderr, "mosaic_probe: unknown flag %s\n", flag.c_str());
        return EXIT_FAILURE;
    }
    if (pathA.empty() || pathB.empty() || !std::isfinite(centerLat) || !std::isfinite(centerLon) ||
        !(boxHalfDeg > 0.0) || !(stepDeg > 0.0) || boxHalfDeg > 10.0) {
        std::fprintf(stderr,
                     "usage: mosaic_probe --a VOL --b VOL [--ida ID --idb ID] "
                     "--clat LAT --clon LON [--elev 0.5 --box 2 --step 0.02 --thresh 10]\n");
        return EXIT_FAILURE;
    }

    VolumeView a, b;
    std::string error;
    if (!loadVolume(pathA, idA, targetElev, a, error)) {
        std::fprintf(stderr, "mosaic_probe: %s\n", error.c_str());
        return EXIT_FAILURE;
    }
    if (!loadVolume(pathB, idB, targetElev, b, error)) {
        std::fprintf(stderr, "mosaic_probe: %s\n", error.c_str());
        return EXIT_FAILURE;
    }
    std::printf("tilt %s %.2f deg, %s %.2f deg\n", a.radarId.c_str(), a.tiltElevationDegrees,
                b.radarId.c_str(), b.tiltElevationDegrees);

    long long cells = 0, echoA = 0, echoB = 0, echoBoth = 0, echoEither = 0;
    double sumDelta = 0.0, sumDeltaSq = 0.0, sumBeamDelta = 0.0;
    long long bothN = 0, bigDisagree = 0;
    std::vector<double> absDeltas;
    std::vector<double> beamDeltas;
    // Seam table: disagreement conditioned on beam-height mismatch, so
    // geometry (different altitudes) separates from calibration (bias at
    // matched beams). Bin edges in meters MSL difference.
    struct SeamBin {
        double maxBeamDelta = 0.0;
        long long n = 0;
        double sum = 0.0;
        double sumSq = 0.0;
    };
    std::vector<SeamBin> seamBins{{1000.0}, {2000.0}, {4000.0}, {1e12}};
    for (double lat = centerLat - boxHalfDeg; lat <= centerLat + boxHalfDeg + 1e-9; lat += stepDeg) {
        for (double lon = centerLon - boxHalfDeg; lon <= centerLon + boxHalfDeg + 1e-9;
             lon += stepDeg) {
            const auto sa = sampleVolume(a, lat, lon, threshDbz);
            const auto sb = sampleVolume(b, lat, lon, threshDbz);
            if (!sa.hasEcho && !sb.hasEcho && (std::isnan(sa.dbz) || std::isnan(sb.dbz))) {
                // Outside both coverages: not a seam data point.
                if (std::isnan(sa.dbz) && std::isnan(sb.dbz)) continue;
            }
            ++cells;
            if (sa.hasEcho) ++echoA;
            if (sb.hasEcho) ++echoB;
            if (sa.hasEcho || sb.hasEcho) ++echoEither;
            if (sa.hasEcho && sb.hasEcho) {
                ++echoBoth;
                const double delta = sa.dbz - sb.dbz;
                sumDelta += delta;
                sumDeltaSq += delta * delta;
                absDeltas.push_back(std::abs(delta));
                if (std::abs(delta) > 5.0) ++bigDisagree;
                const double beamDelta = sa.beamMslMeters - sb.beamMslMeters;
                sumBeamDelta += beamDelta;
                beamDeltas.push_back(std::abs(beamDelta));
                for (auto& bin : seamBins) {
                    if (std::abs(beamDelta) <= bin.maxBeamDelta) {
                        ++bin.n;
                        bin.sum += delta;
                        bin.sumSq += delta * delta;
                        break;
                    }
                }
                ++bothN;
            }
        }
    }
    std::sort(absDeltas.begin(), absDeltas.end());
    std::sort(beamDeltas.begin(), beamDeltas.end());
    const auto percentile = [](const std::vector<double>& sorted, double q) {
        if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
        return sorted[static_cast<std::size_t>(q * (sorted.size() - 1))];
    };
    const double bias = bothN > 0 ? sumDelta / bothN : std::numeric_limits<double>::quiet_NaN();
    const double rmse =
        bothN > 0 ? std::sqrt(sumDeltaSq / bothN) : std::numeric_limits<double>::quiet_NaN();
    std::printf("cells %lld echoA %lld echoB %lld either %lld both %lld\n", cells, echoA, echoB,
                echoEither, echoBoth);
    std::printf("coverage gain vs A alone: +%.1f%% cells with echo\n",
                echoA > 0 ? 100.0 * (echoEither - echoA) / echoA : 0.0);
    std::printf("both-echo delta dB: bias %+.2f rmse %.2f p50 %.2f p90 %.2f over5dB %.1f%% (n=%lld)\n",
                bias, rmse, percentile(absDeltas, 0.5), percentile(absDeltas, 0.9),
                bothN > 0 ? 100.0 * bigDisagree / bothN : 0.0, bothN);
    std::printf("beam MSL mismatch m: mean %+.0f p50 %.0f p90 %.0f\n",
                bothN > 0 ? sumBeamDelta / bothN : 0.0, percentile(beamDeltas, 0.5),
                percentile(beamDeltas, 0.9));
    std::printf("seam by beam mismatch: [beam<=m] n bias rmse\n");
    for (const auto& bin : seamBins) {
        const double edge = bin.maxBeamDelta > 1e11 ? 1e12 : bin.maxBeamDelta;
        if (bin.n > 0) {
            std::printf("  <=%.0f n=%lld bias=%+.2f rmse=%.2f\n", edge, bin.n, bin.sum / bin.n,
                        std::sqrt(bin.sumSq / bin.n));
        } else {
            std::printf("  <=%.0f n=0\n", edge);
        }
    }
    return EXIT_SUCCESS;
}
