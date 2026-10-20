// Mosaic seam probe (offline prototype): two synced single-radar volumes,
// one geographic grid, matched-tilt reflectivity from each, and the seam
// numbers a mosaic commit needs: coverage gain, both-echo disagreement
// (bias/RMSE/|d|>5 dB share), and beam-height mismatch where both see echo.
// --cappi HMSL swaps the matched tilt for altitude-matched sampling (each
// radar's tilt closest to HMSL MSL per cell) to score CAPPI compositing.
// --footprint own|common averages every gate whose beam volume overlaps the
// cell instead of one gate (resolution-aware blending, with --footprint-scale
// to widen the window) and --blocks splits the paired delta's variance by
// scale, so an averaging win can be told apart from real agreement: a delta
// that is spatially white is representativeness error, a coherent one is
// altitude, calibration or microphysics. Same decode + geometry as the live
// canvas (`decodeLevel2Archive`, `rangeAndBearing`, `gateIndexForSlantRange`,
// `nativeValue`), so the numbers describe production sampling, not a toy
// model. Deterministic: fixed grid, no network, exit 0 with a report,
// nonzero on misuse or decode failure.

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
#include <map>
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
    // CAPPI mode: azimuth-sorted (azimuth, ray index) per tilt elevation,
    // so each cell can sample the tilt whose beam rides closest to the
    // target altitude instead of one matched tilt for the whole grid.
    std::map<double, std::vector<std::pair<double, std::size_t>>> tiltRays;
};

struct CellSample {
    bool hasEcho = false;
    double dbz = std::numeric_limits<double>::quiet_NaN();
    double beamMslMeters = std::numeric_limits<double>::quiet_NaN();
};

// Per-gate footprint average: every gate whose beam volume overlaps the
// cell's cross-track / along-track window, equal weight. Censored gates
// (0/1 = missing/below detection) are counted but never averaged, so the
// mean is the detected-part mean and the valid share says how much of the
// volume that was. No interpolation, no fill.
struct FootprintSample {
    bool valid = false;
    double dbz = std::numeric_limits<double>::quiet_NaN();
    double beamMslMeters = std::numeric_limits<double>::quiet_NaN();
    long long gatesValid = 0;
    long long gatesTotal = 0;
    double distanceMeters = 0.0;
};

// Half-widths of one gate's beam volume projected on the ground: cross-track
// from the azimuth beamwidth at this range, along-track from gate spacing.
struct FootprintExtent {
    double along = 0.0;
    double radial = 0.0;
};

// Range/bearing to the cell plus the nearest ray in a tilt index. Shared by
// the point and footprint samplers so both apply identical azimuth QC.
struct RayHit {
    bool ok = false;
    std::size_t rayIndex = 0;
    double distanceMeters = 0.0;
    double bearingDegrees = 0.0;
    double slantMeters = 0.0;
    double azimuthDiffDegrees = 0.0;
};

RayHit locateRay(const VolumeView& view,
                 const std::vector<std::pair<double, std::size_t>>& index, double lat,
                 double lon) {
    using namespace cappedweather;
    RayHit hit;
    if (index.empty()) return hit;
    const auto location = rangeAndBearing({view.siteLat, view.siteLon}, {lat, lon});
    if (!std::isfinite(location.distanceMeters) || location.distanceMeters < 1000.0 ||
        location.distanceMeters > 460000.0)
        return hit;
    const auto circularDifference = [azimuth = location.bearingDegrees](double candidate) {
        const double direct = std::abs(candidate - azimuth);
        return std::min(direct, 360.0 - direct);
    };
    const auto candidate =
        std::lower_bound(index.cbegin(), index.cend(), location.bearingDegrees,
                         [](const auto& indexed, double target) { return indexed.first < target; });
    auto nearest = candidate == index.cend() ? index.cbegin() : candidate;
    auto nearestDiff = circularDifference(nearest->first);
    const auto previous =
        candidate == index.cbegin() ? std::prev(index.cend()) : std::prev(candidate);
    if (circularDifference(previous->first) < nearestDiff) {
        nearest = previous;
        nearestDiff = circularDifference(previous->first);
    }
    const auto& ray = view.chunk.rays.at(nearest->second);
    hit.ok = true;
    hit.rayIndex = nearest->second;
    hit.distanceMeters = location.distanceMeters;
    hit.bearingDegrees = location.bearingDegrees;
    hit.azimuthDiffDegrees = nearestDiff;
    hit.slantMeters = slantRangeForSurfaceDistance(location.distanceMeters, ray.elevationDegrees);
    return hit;
}

// A radar only samples this cell if a real ray points at its azimuth.
bool rayCoversBearing(const cappedweather::DecodedRay& ray, double azimuthDiffDegrees) {
    return azimuthDiffDegrees <= std::max(1.5, ray.azimuthResolutionDegrees * 1.5);
}

FootprintExtent footprintForRay(const cappedweather::DecodedRay& ray, double surfaceDistanceMeters) {
    constexpr std::size_t kProduct = static_cast<std::size_t>(cappedweather::ProductKind::Reflectivity);
    FootprintExtent extent;
    extent.along = 0.5 * ray.azimuthResolutionDegrees * 3.14159265358979323846 / 180.0 *
                   surfaceDistanceMeters;
    extent.radial =
        0.5 * static_cast<double>(ray.gateSpacingByProduct.at(kProduct));
    return extent;
}

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
    // Per-tilt azimuth index for CAPPI mode: same 0.05 deg grouping the
    // matched tilt uses, so both modes sample identical ray sets.
    for (std::size_t i = 0; i < out.chunk.rays.size(); ++i) {
        const auto& ray = out.chunk.rays[i];
        if (!ray.has(cappedweather::ProductKind::Reflectivity)) continue;
        bool placed = false;
        for (auto& [elev, index] : out.tiltRays) {
            if (std::abs(ray.elevationDegrees - elev) <= 0.05) {
                index.emplace_back(ray.azimuthDegrees, i);
                placed = true;
                break;
            }
        }
        if (!placed) {
            out.tiltRays[ray.elevationDegrees].emplace_back(ray.azimuthDegrees, i);
        }
    }
    for (auto& [elev, index] : out.tiltRays) {
        (void)elev;
        std::sort(index.begin(), index.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    long long tiltGates = 0, tiltEcho = 0;
    constexpr std::size_t kRef = static_cast<std::size_t>(cappedweather::ProductKind::Reflectivity);
    for (std::size_t i = 0; i < out.chunk.rays.size(); ++i) {
        const auto& ray = out.chunk.rays[i];
        if (!ray.has(cappedweather::ProductKind::Reflectivity)) continue;
        if (std::abs(ray.elevationDegrees - tiltElev) > 0.05) continue;
        out.rayIndex.emplace_back(ray.azimuthDegrees, i);
        for (const auto encoded : ray.fields.at(kRef)) {
            ++tiltGates;
            const auto value = cappedweather::nativeValue(
                encoded, ray.scaleByProduct.at(kRef), ray.offsetByProduct.at(kRef));
            if (value.has_value() && *value >= 10.0) ++tiltEcho;
        }
    }
    if (out.rayIndex.empty()) {
        error = "matched tilt is empty in " + path;
        return false;
    }
    std::printf("tilt %.2f deg: %zu rays, %lld gates, %.1f%% echo>=10dBZ\n", tiltElev,
                out.rayIndex.size(), tiltGates,
                tiltGates > 0 ? 100.0 * tiltEcho / tiltGates : 0.0);
    if (!out.rayIndex.empty()) {
        // Azimuth coverage in 5-degree bins: a sector scan (or truncated
        // cut) shows up as missing bins and explains sparse grids without
        // blaming the sampler. Bins (not max-gap) so overlapping duplicate
        // sweeps cannot fake coverage or a fake gap.
        int coveredBins = 0;
        for (int bin = 0; bin < 72; ++bin) {
            const double lo = bin * 5.0, hi = lo + 5.0;
            for (const auto& [az, unused] : out.rayIndex) {
                (void)unused;
                double wrapped = az;
                while (wrapped < 0.0) wrapped += 360.0;
                while (wrapped >= 360.0) wrapped -= 360.0;
                if (wrapped >= lo && wrapped < hi) {
                    ++coveredBins;
                    break;
                }
            }
        }
        std::printf("tilt %.2f deg: %d/72 five-degree bins covered\n", tiltElev, coveredBins);
    }
    std::sort(out.rayIndex.begin(), out.rayIndex.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return true;
}

bool sampleTiltRays(const VolumeView& view,
                    const std::vector<std::pair<double, std::size_t>>& index, double lat,
                    double lon, double& beamMslMeters, double& dbz) {
    using namespace cappedweather;
    const auto hit = locateRay(view, index, lat, lon);
    if (!hit.ok) return false;
    constexpr std::size_t kProduct = static_cast<std::size_t>(ProductKind::Reflectivity);
    const auto& ray = view.chunk.rays.at(hit.rayIndex);
    if (!rayCoversBearing(ray, hit.azimuthDiffDegrees)) return false;
    const auto gate = gateIndexForSlantRange(hit.slantMeters, ray.firstGateByProduct.at(kProduct),
                                             ray.gateSpacingByProduct.at(kProduct),
                                             ray.fields.at(kProduct).size());
    if (!gate) return false;
    const auto value = nativeValue(ray.fields.at(kProduct).at(*gate),
                                   ray.scaleByProduct.at(kProduct), ray.offsetByProduct.at(kProduct));
    if (!value.has_value()) return false;
    const BeamGeometry beam = standardBeamGeometry(hit.slantMeters, ray.elevationDegrees);
    beamMslMeters = view.siteMslMeters + beam.heightAboveRadarMeters;
    dbz = *value;
    return true;
}

// Footprint average inside one tilt: every gate whose beam volume overlaps
// the cell, equal weight. Uses the same azimuth QC as the point sampler, so
// the only difference between the two is the averaging window.
FootprintSample sampleFootprint(const VolumeView& view,
                                const std::vector<std::pair<double, std::size_t>>& index,
                                double lat, double lon, double alongHalfMeters,
                                double radialHalfMeters) {
    using namespace cappedweather;
    constexpr std::size_t kProduct = static_cast<std::size_t>(ProductKind::Reflectivity);
    FootprintSample out;
    const auto hit = locateRay(view, index, lat, lon);
    if (!hit.ok) return out;
    const auto& anchor = view.chunk.rays.at(hit.rayIndex);
    if (!rayCoversBearing(anchor, hit.azimuthDiffDegrees)) return out;
    out.distanceMeters = hit.distanceMeters;
    out.beamMslMeters =
        view.siteMslMeters + standardBeamGeometry(hit.slantMeters, anchor.elevationDegrees)
                                 .heightAboveRadarMeters;
    // Cross-track window in degrees at the cell's range. Beyond 180 deg the
    // window covers every ray, which is the honest limit of the model.
    const double halfAzDegrees =
        std::min(180.0, (alongHalfMeters / std::max(hit.distanceMeters, 1.0)) * 180.0 /
                            3.14159265358979323846);
    double sum = 0.0;
    long long valid = 0, total = 0;
    for (const auto& [azimuth, rayIndex] : index) {
        const double direct = std::abs(azimuth - hit.bearingDegrees);
        if (std::min(direct, 360.0 - direct) > halfAzDegrees) continue;
        const auto& ray = view.chunk.rays.at(rayIndex);
        const auto& field = ray.fields.at(kProduct);
        const double spacing = static_cast<double>(ray.gateSpacingByProduct.at(kProduct));
        if (!(spacing > 0.0)) continue;
        const auto slant =
            slantRangeForSurfaceDistance(hit.distanceMeters, ray.elevationDegrees);
        const auto center =
            gateIndexForSlantRange(slant, ray.firstGateByProduct.at(kProduct),
                                   ray.gateSpacingByProduct.at(kProduct), field.size());
        if (!center) continue;
        const long long halfGates =
            std::max(0LL, static_cast<long long>(std::floor(radialHalfMeters / spacing + 1e-9)));
        const long long lo = static_cast<long long>(*center) - halfGates;
        const long long hi = static_cast<long long>(*center) + halfGates;
        for (long long gate = lo; gate <= hi; ++gate) {
            if (gate < 0 || gate >= static_cast<long long>(field.size())) continue;
            ++total;
            const auto value = nativeValue(field.at(static_cast<std::size_t>(gate)),
                                           ray.scaleByProduct.at(kProduct),
                                           ray.offsetByProduct.at(kProduct));
            if (!value.has_value()) continue;
            sum += *value;
            ++valid;
        }
    }
    out.gatesTotal = total;
    out.gatesValid = valid;
    if (valid > 0) {
        out.valid = true;
        out.dbz = sum / static_cast<double>(valid);
    }
    return out;
}

CellSample sampleVolume(const VolumeView& view, double lat, double lon, double echoThresholdDbz) {
    CellSample sample;
    double beamMsl = std::numeric_limits<double>::quiet_NaN();
    double dbz = std::numeric_limits<double>::quiet_NaN();
    if (!sampleTiltRays(view, view.rayIndex, lat, lon, beamMsl, dbz)) return sample;
    sample.beamMslMeters = beamMsl;
    sample.dbz = dbz;
    sample.hasEcho = dbz >= echoThresholdDbz;
    return sample;
}

// Tilt selection shared by the point and footprint CAPPI samplers: the tilt
// whose beam rides closest to target MSL at this cell. Carries the tilt index
// and the cell's footprint extent so averaging happens in the same tilt the
// point sample would have used.
struct CappiPick {
    bool ok = false;
    const std::vector<std::pair<double, std::size_t>>* index = nullptr;
    double beamMslMeters = std::numeric_limits<double>::quiet_NaN();
    double dbz = std::numeric_limits<double>::quiet_NaN();
    double distanceMeters = 0.0;
    FootprintExtent extent;
};

CappiPick selectCappiTilt(const VolumeView& view, double lat, double lon,
                          double targetMslMeters) {
    CappiPick pick;
    double bestMiss = std::numeric_limits<double>::infinity();
    for (const auto& [elev, index] : view.tiltRays) {
        (void)elev;
        double beamMsl = std::numeric_limits<double>::quiet_NaN();
        double dbz = std::numeric_limits<double>::quiet_NaN();
        if (!sampleTiltRays(view, index, lat, lon, beamMsl, dbz)) continue;
        const double miss = std::abs(beamMsl - targetMslMeters);
        if (miss < bestMiss) {
            bestMiss = miss;
            pick.ok = true;
            pick.index = &index;
            pick.beamMslMeters = beamMsl;
            pick.dbz = dbz;
            const auto hit = locateRay(view, index, lat, lon);
            pick.distanceMeters = hit.distanceMeters;
            if (hit.ok) {
                pick.extent = footprintForRay(view.chunk.rays.at(hit.rayIndex),
                                              hit.distanceMeters);
            }
        }
    }
    return pick;
}

CellSample sampleCappi(const VolumeView& view, double lat, double lon, double echoThresholdDbz,
                       double targetMslMeters) {
    CellSample sample;
    const auto pick = selectCappiTilt(view, lat, lon, targetMslMeters);
    if (!pick.ok) return sample;
    sample.beamMslMeters = pick.beamMslMeters;
    sample.dbz = pick.dbz;
    sample.hasEcho = pick.dbz >= echoThresholdDbz;
    return sample;
}

// Scale decomposition of the paired delta. If the disagreement is
// representativeness error (sub-footprint noise) it is spatially white, so
// block means lose variance as 1/n and between-block variance tracks the
// white-noise prediction. Coherent structure (calibration, altitude,
// registration) keeps between-block variance above that prediction. This is
// the honest test for "does averaging just hide the mismatch".
void printBlockTable(const std::vector<int>& ix, const std::vector<int>& iy,
                     const std::vector<double>& delta, const char* label) {
    if (delta.empty()) {
        std::printf("scale decomposition (%s): no both-echo cells\n", label);
        return;
    }
    std::printf("scale decomposition (%s): delta=A-B on both-echo cells\n", label);
    std::printf("  block_n blocks rmse_blocks within_var between_var white_pred between/white\n");
    for (const int n : {1, 2, 4, 8, 16}) {
        struct Bin {
            long long n = 0;
            double sum = 0.0;
            double sumSq = 0.0;
        };
        std::map<std::pair<int, int>, Bin> bins;
        for (std::size_t i = 0; i < delta.size(); ++i) {
            auto& bin = bins[{ix[i] / n, iy[i] / n}];
            ++bin.n;
            bin.sum += delta[i];
            bin.sumSq += delta[i] * delta[i];
        }
        long long total = 0;
        double grandSum = 0.0;
        for (const auto& entry : bins) {
            total += entry.second.n;
            grandSum += entry.second.sum;
        }
        if (total == 0) continue;
        const double grand = grandSum / static_cast<double>(total);
        double within = 0.0, between = 0.0, whitePredicted = 0.0;
        for (const auto& entry : bins) {
            const auto& bin = entry.second;
            const double mean = bin.sum / static_cast<double>(bin.n);
            double variance = bin.sumSq / static_cast<double>(bin.n) - mean * mean;
            if (variance < 0.0) variance = 0.0;
            const double share = static_cast<double>(bin.n) / static_cast<double>(total);
            within += variance * share;
            between += share * (mean - grand) * (mean - grand);
            whitePredicted += variance / static_cast<double>(total);
        }
        std::printf("  %2d %6zu %.2f %.2f %.2f %.2f %.2f\n", n, bins.size(),
                    std::sqrt(between + grand * grand), within, between, whitePredicted,
                    whitePredicted > 0.0 ? between / whitePredicted : 0.0);
    }
    std::printf(
        "  (white_pred = between_var expected if the delta were spatially white given the "
        "measured within-block variance)\n");
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::string pathA, pathB, idA, idB;
    double targetElev = 0.5, boxHalfDeg = 2.0, stepDeg = 0.02, threshDbz = 10.0;
    double cappiMslMeters = std::numeric_limits<double>::quiet_NaN();
    double advectKm = std::numeric_limits<double>::quiet_NaN();
    double centerLat = std::numeric_limits<double>::quiet_NaN();
    double centerLon = std::numeric_limits<double>::quiet_NaN();
    // Per-gate / resolution-aware blending modes. own = each radar averaged
    // over its own beam footprint; common = both averaged over the coarser of
    // the two footprints at that cell, the fair matched-scale comparison.
    enum class FootprintMode { None, Own, Common };
    FootprintMode footprintMode = FootprintMode::None;
    // Multiplies the instrument footprint half-widths. 1 = the sampling
    // footprint itself (ray spacing x gate spacing); 2-3 covers the physical
    // 0.925 deg beam, which is wider than the 0.5 deg super-res sampling.
    double footprintScale = 1.0;
    bool blockTable = false;
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
            flag == "--clat" || flag == "--clon" || flag == "--cappi" || flag == "--advect") {
            double* out = flag == "--elev"    ? &targetElev
                          : flag == "--box"   ? &boxHalfDeg
                          : flag == "--step"  ? &stepDeg
                          : flag == "--thresh" ? &threshDbz
                          : flag == "--cappi"  ? &cappiMslMeters
                          : flag == "--advect" ? &advectKm
                          : flag == "--clat"   ? &centerLat
                                               : &centerLon;
            if (!needDouble(*out)) return EXIT_FAILURE;
            continue;
        }
        if (flag == "--footprint") {
            std::string text;
            if (!need(text)) return EXIT_FAILURE;
            if (text == "own") {
                footprintMode = FootprintMode::Own;
            } else if (text == "common") {
                footprintMode = FootprintMode::Common;
            } else {
                std::fprintf(stderr, "mosaic_probe: --footprint needs own|common\n");
                return EXIT_FAILURE;
            }
            continue;
        }
        if (flag == "--footprint-scale") {
            if (!needDouble(footprintScale)) return EXIT_FAILURE;
            continue;
        }
        if (flag == "--blocks") {
            blockTable = true;
            ++i;
            continue;
        }
        std::fprintf(stderr, "mosaic_probe: unknown flag %s\n", flag.c_str());
        return EXIT_FAILURE;
    }
    if (pathA.empty() || pathB.empty() || !std::isfinite(centerLat) || !std::isfinite(centerLon) ||
        !(boxHalfDeg > 0.0) || !(stepDeg > 0.0) || boxHalfDeg > 10.0) {
        std::fprintf(stderr,
                     "usage: mosaic_probe --a VOL --b VOL [--ida ID --idb ID] "
                     "--clat LAT --clon LON [--elev 0.5 --box 2 --step 0.02 --thresh 10]\n"
                     "  --cappi HMSL: sample each radar's tilt closest to HMSL MSL instead of one\n"
                     "  matched tilt (altitude-matched compositing probe)\n"
                     "  --advect KM: lag table shifting radar B up to KM km (needs --cappi)\n"
                     "  --footprint own|common: average every gate whose beam volume overlaps\n"
                     "  the cell instead of one gate (needs --cappi; common = both radars at the\n"
                     "  coarser footprint, the matched-scale comparison)\n"
                     "  --blocks: scale decomposition of the paired delta (white vs coherent)\n"
                     "  --footprint-scale N: multiply the instrument footprint (default 1; 2-3\n"
                     "  approximates the physical 0.925 deg beam)\n");
        return EXIT_FAILURE;
    }
    if (!(footprintScale > 0.0) || footprintScale > 16.0) {
        std::fprintf(stderr, "mosaic_probe: --footprint-scale needs (0, 16]\n");
        return EXIT_FAILURE;
    }
    if (footprintMode != FootprintMode::None && !std::isfinite(cappiMslMeters)) {
        std::fprintf(stderr,
                     "mosaic_probe: --footprint averages inside the CAPPI-selected tilt and "
                     "needs --cappi HMSL\n");
        return EXIT_FAILURE;
    }
    if (std::isfinite(cappiMslMeters) && cappiMslMeters <= 0.0) {
        std::fprintf(stderr, "mosaic_probe: --cappi needs a positive MSL altitude in meters\n");
        return EXIT_FAILURE;
    }
    if (std::isfinite(advectKm) && (advectKm <= 0.0 || advectKm > 10.0)) {
        std::fprintf(stderr, "mosaic_probe: --advect needs a radius in km (0, 10]\n");
        return EXIT_FAILURE;
    }
    if (std::isfinite(advectKm) && !std::isfinite(cappiMslMeters)) {
        std::fprintf(stderr, "mosaic_probe: --advect currently rides --cappi sampling\n");
        return EXIT_FAILURE;
    }
    if (std::isfinite(advectKm) && footprintMode != FootprintMode::None) {
        std::fprintf(stderr,
                     "mosaic_probe: --advect scores point CAPPI; run it without --footprint\n");
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
    const bool cappi = std::isfinite(cappiMslMeters);
    if (cappi) std::printf("cappi target %.0f m MSL\n", cappiMslMeters);

    long long cells = 0, echoA = 0, echoB = 0, echoBoth = 0, echoEither = 0;
    double sumDelta = 0.0, sumDeltaSq = 0.0, sumBeamDelta = 0.0;
    long long bothN = 0, bigDisagree = 0;
    std::vector<double> absDeltas;
    std::vector<double> beamDeltas;
    // Paired same-population comparison (CAPPI mode only): on cells where
    // the matched tilt ALSO sees both echoes, score both methods'
    // absolute deltas. Same cells, so population shift cannot flatter
    // either side; win rate is the share with smaller CAPPI error.
    std::vector<double> pairedBase;
    std::vector<double> pairedCappi;
    struct PairedCell {
        double lat = 0.0;
        double lon = 0.0;
        double aDbz = 0.0;
        double bDbz = 0.0;
    };
    std::vector<PairedCell> pairedCells;
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
    // Footprint-mode accumulators: identical-cell 3-way paired deltas (point
    // CAPPI vs own-footprint vs common-footprint), the >=90%-valid variant
    // that censoring cannot flatter, and the geometry actually averaged.
    std::vector<double> pairedPoint, pairedOwn, pairedCommon;
    std::vector<double> strictPoint, strictOwn, strictCommon;
    long long fpOwnBoth = 0, fpCommonBoth = 0, ownSamplesA = 0, ownSamplesB = 0;
    double ownAlongA = 0.0, ownRadialA = 0.0, ownAlongB = 0.0, ownRadialB = 0.0;
    long long gatesA = 0, gatesTotalA = 0, gatesB = 0, gatesTotalB = 0;
    std::vector<int> blockIx, blockIy;
    std::vector<double> blockDelta;
    for (double lat = centerLat - boxHalfDeg; lat <= centerLat + boxHalfDeg + 1e-9; lat += stepDeg) {
        for (double lon = centerLon - boxHalfDeg; lon <= centerLon + boxHalfDeg + 1e-9;
             lon += stepDeg) {
            // Point CAPPI pick is shared by every mode: it fixes the tilt
            // (the altitude match) that a footprint average then samples
            // inside, so footprint numbers differ from the point baseline
            // only by the averaging window.
            CappiPick pickA, pickB;
            CellSample pointA, pointB, sa, sb;
            FootprintSample ownA, ownB, commonA, commonB;
            if (cappi) {
                pickA = selectCappiTilt(a, lat, lon, cappiMslMeters);
                pickB = selectCappiTilt(b, lat, lon, cappiMslMeters);
                pointA = CellSample{pickA.ok && pickA.dbz >= threshDbz, pickA.dbz,
                                    pickA.beamMslMeters};
                pointB = CellSample{pickB.ok && pickB.dbz >= threshDbz, pickB.dbz,
                                    pickB.beamMslMeters};
            } else {
                pointA = sampleVolume(a, lat, lon, threshDbz);
                pointB = sampleVolume(b, lat, lon, threshDbz);
            }
            sa = pointA;
            sb = pointB;
            if (footprintMode != FootprintMode::None) {
                if (pickA.ok && pickA.index) {
                    ownA = sampleFootprint(a, *pickA.index, lat, lon,
                                           pickA.extent.along * footprintScale,
                                           pickA.extent.radial * footprintScale);
                }
                if (pickB.ok && pickB.index) {
                    ownB = sampleFootprint(b, *pickB.index, lat, lon,
                                           pickB.extent.along * footprintScale,
                                           pickB.extent.radial * footprintScale);
                }
                const double alongCommon =
                    footprintScale * std::max(pickA.extent.along, pickB.extent.along);
                const double radialCommon =
                    footprintScale * std::max(pickA.extent.radial, pickB.extent.radial);
                if (pickA.ok && pickA.index) {
                    commonA = sampleFootprint(a, *pickA.index, lat, lon, alongCommon, radialCommon);
                    ownAlongA += pickA.extent.along;
                    ownRadialA += pickA.extent.radial;
                    if (ownA.valid) ++ownSamplesA;
                }
                if (pickB.ok && pickB.index) {
                    commonB = sampleFootprint(b, *pickB.index, lat, lon, alongCommon, radialCommon);
                    ownAlongB += pickB.extent.along;
                    ownRadialB += pickB.extent.radial;
                    if (ownB.valid) ++ownSamplesB;
                }
                const bool useCommon = footprintMode == FootprintMode::Common;
                const auto& fa = useCommon ? commonA : ownA;
                const auto& fb = useCommon ? commonB : ownB;
                sa = CellSample{fa.valid && fa.dbz >= threshDbz, fa.dbz, fa.beamMslMeters};
                sb = CellSample{fb.valid && fb.dbz >= threshDbz, fb.dbz, fb.beamMslMeters};
                if (ownA.valid && ownB.valid && ownA.dbz >= threshDbz && ownB.dbz >= threshDbz) {
                    ++fpOwnBoth;
                }
                if (commonA.valid && commonB.valid && commonA.dbz >= threshDbz &&
                    commonB.dbz >= threshDbz) {
                    ++fpCommonBoth;
                }
                if (fa.valid) {
                    gatesA += fa.gatesValid;
                    gatesTotalA += fa.gatesTotal;
                }
                if (fb.valid) {
                    gatesB += fb.gatesValid;
                    gatesTotalB += fb.gatesTotal;
                }
            }
            // Outside both coverages (no gate from either radar): not data.
            if (std::isnan(sa.dbz) && std::isnan(sb.dbz)) continue;
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
                if (cappi) {
                    const auto ba = sampleVolume(a, lat, lon, threshDbz);
                    const auto bb = sampleVolume(b, lat, lon, threshDbz);
                    if (ba.hasEcho && bb.hasEcho) {
                        pairedBase.push_back(std::abs(ba.dbz - bb.dbz));
                        pairedCappi.push_back(std::abs(delta));
                        pairedCells.push_back({lat, lon, sa.dbz, sb.dbz});
                    }
                }
                if (footprintMode != FootprintMode::None && pointA.hasEcho && pointB.hasEcho &&
                    ownA.valid && ownB.valid && ownA.dbz >= threshDbz && ownB.dbz >= threshDbz &&
                    commonA.valid && commonB.valid && commonA.dbz >= threshDbz &&
                    commonB.dbz >= threshDbz) {
                    // Identical cells for all three estimators: no population
                    // shift can flatter the footprint numbers. Signed deltas
                    // so RMSE and bias both come out of the same sample.
                    const double pointDelta = pickA.dbz - pickB.dbz;
                    const double ownDelta = ownA.dbz - ownB.dbz;
                    const double commonDelta = commonA.dbz - commonB.dbz;
                    pairedPoint.push_back(pointDelta);
                    pairedOwn.push_back(ownDelta);
                    pairedCommon.push_back(commonDelta);
                    const double shareA = commonA.gatesTotal > 0
                                              ? static_cast<double>(commonA.gatesValid) / commonA.gatesTotal
                                              : 0.0;
                    const double shareB = commonB.gatesTotal > 0
                                              ? static_cast<double>(commonB.gatesValid) / commonB.gatesTotal
                                              : 0.0;
                    if (shareA >= 0.9 && shareB >= 0.9) {
                        strictPoint.push_back(pointDelta);
                        strictOwn.push_back(ownDelta);
                        strictCommon.push_back(commonDelta);
                    }
                }
                if (blockTable) {
                    blockIx.push_back(static_cast<int>(
                        std::llround((lon - (centerLon - boxHalfDeg)) / stepDeg)));
                    blockIy.push_back(static_cast<int>(
                        std::llround((lat - (centerLat - boxHalfDeg)) / stepDeg)));
                    blockDelta.push_back(delta);
                }
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
    const char* edges[] = {"<=1000", "<=2000", "<=4000", "<=rest"};
    for (std::size_t i = 0; i < seamBins.size(); ++i) {
        const auto& bin = seamBins[i];
        if (bin.n > 0) {
            std::printf("  %s n=%lld bias=%+.2f rmse=%.2f\n", edges[i], bin.n, bin.sum / bin.n,
                        std::sqrt(bin.sumSq / bin.n));
        } else {
            std::printf("  %s n=0\n", edges[i]);
        }
    }
    if (blockTable) {
        printBlockTable(blockIx, blockIy, blockDelta,
                        footprintMode == FootprintMode::None  ? "point cappi"
                        : footprintMode == FootprintMode::Own ? "own footprint"
                                                              : "common footprint");
    }
    if (footprintMode != FootprintMode::None) {
        std::printf("footprint scale: %.2fx instrument footprint\n", footprintScale);
        std::printf("footprint mode: %s\n",
                    footprintMode == FootprintMode::Common
                        ? "common (both radars at the coarser footprint at each cell)"
                        : "own (each radar over its own beam volume)");
        std::printf("  own extent (mean, km): %s along %.2f radial %.2f | %s along %.2f radial "
                    "%.2f\n",
                    a.radarId.c_str(), ownSamplesA ? ownAlongA / ownSamplesA / 1000.0 : 0.0,
                    ownSamplesA ? ownRadialA / ownSamplesA / 1000.0 : 0.0, b.radarId.c_str(),
                    ownSamplesB ? ownAlongB / ownSamplesB / 1000.0 : 0.0,
                    ownSamplesB ? ownRadialB / ownSamplesB / 1000.0 : 0.0);
        std::printf("  gates summed: %s %lld valid %.1f%% | %s %lld valid %.1f%%\n",
                    a.radarId.c_str(), gatesTotalA,
                    gatesTotalA > 0 ? 100.0 * gatesA / gatesTotalA : 0.0, b.radarId.c_str(),
                    gatesTotalB, gatesTotalB > 0 ? 100.0 * gatesB / gatesTotalB : 0.0);
        std::printf("  both-echo cells by estimator: own %lld common %lld\n", fpOwnBoth,
                    fpCommonBoth);
        const auto printThreeWay = [](const char* tag, const std::vector<double>& point,
                                      const std::vector<double>& own,
                                      const std::vector<double>& common) {
            const std::size_t n = std::min({point.size(), own.size(), common.size()});
            if (n == 0) {
                std::printf("  paired %s n=0\n", tag);
                return;
            }
            double pointSq = 0.0, ownSq = 0.0, commonSq = 0.0;
            double pointSum = 0.0, ownSum = 0.0, commonSum = 0.0;
            long long ownWins = 0, commonWins = 0;
            for (std::size_t i = 0; i < n; ++i) {
                pointSq += point[i] * point[i];
                ownSq += own[i] * own[i];
                commonSq += common[i] * common[i];
                pointSum += point[i];
                ownSum += own[i];
                commonSum += common[i];
                if (std::abs(own[i]) < std::abs(point[i])) ++ownWins;
                if (std::abs(common[i]) < std::abs(point[i])) ++commonWins;
            }
            const double pointRmse = std::sqrt(pointSq / static_cast<double>(n));
            const double ownRmse = std::sqrt(ownSq / static_cast<double>(n));
            const double commonRmse = std::sqrt(commonSq / static_cast<double>(n));
            const double denom = pointRmse > 0.0 ? pointRmse : 1.0;
            std::printf(
                "  paired %s (n=%zu): point rmse %.2f bias %+.2f | own rmse %.2f (%+.1f%%) | "
                "common rmse %.2f (%+.1f%%)\n",
                tag, n, pointRmse, pointSum / static_cast<double>(n), ownRmse,
                100.0 * (ownRmse - pointRmse) / denom, commonRmse,
                100.0 * (commonRmse - pointRmse) / denom);
            std::printf("  paired %s winrate vs point: own %.1f%% common %.1f%%\n", tag,
                        100.0 * ownWins / static_cast<double>(n),
                        100.0 * commonWins / static_cast<double>(n));
        };
        printThreeWay("all", pairedPoint, pairedOwn, pairedCommon);
        printThreeWay(">=90%valid", strictPoint, strictOwn, strictCommon);
    }
    if (cappi) {
        // Same-population verdict: paired RMSE on identical cells plus the
        // share where altitude matching strictly wins. Immune to the
        // population shift between the baseline and CAPPI both-echo sets.
        const std::size_t n = std::min(pairedBase.size(), pairedCappi.size());
        double baseSq = 0.0, cappiSq = 0.0;
        long long wins = 0;
        for (std::size_t i = 0; i < n; ++i) {
            baseSq += pairedBase[i] * pairedBase[i];
            cappiSq += pairedCappi[i] * pairedCappi[i];
            if (pairedCappi[i] < pairedBase[i]) ++wins;
        }
        // Footprint mode prints its own 3-way table against the point CAPPI
        // baseline; this one would relabel footprint errors as "cappi".
        if (footprintMode == FootprintMode::None) {
            std::printf(
                "paired same-cell (n=%zu): base rmse %.2f cappi rmse %.2f winrate %.1f%%\n", n,
                n > 0 ? std::sqrt(baseSq / n) : 0.0, n > 0 ? std::sqrt(cappiSq / n) : 0.0,
                n > 0 ? 100.0 * wins / n : 0.0);
        }
        // Advection lag search: resample radar B shifted by (dx,dy) km and
        // re-score the paired cells. If the residual is time sync, the
        // minimum sits near storm-motion x volume separation, and its
        // depth bounds what any sync step can recover. A fixed population
        // per shift would hide edge losses, so n rides along.
        if (std::isfinite(advectKm) && !pairedCells.empty()) {
            const double kmPerDegLat = 111.32;
            const double kmPerDegLon = 111.32 * std::cos(centerLat * 3.14159265358979323846 / 180.0);
            double bestRmse = std::numeric_limits<double>::infinity();
            double bestDx = 0.0, bestDy = 0.0;
            long long bestN = 0;
            std::printf("advect lag table: dxE_km dyN_km n rmse\n");
            for (double dy = -advectKm; dy <= advectKm + 1e-9; dy += 1.0) {
                for (double dx = -advectKm; dx <= advectKm + 1e-9; dx += 1.0) {
                    double sumSq = 0.0;
                    long long count = 0;
                    for (const auto& cell : pairedCells) {
                        const auto shifted = sampleCappi(b, cell.lat - dy / kmPerDegLat,
                                                         cell.lon - dx / kmPerDegLon, threshDbz,
                                                         cappiMslMeters);
                        if (!shifted.hasEcho) continue;
                        const double delta = cell.aDbz - shifted.dbz;
                        sumSq += delta * delta;
                        ++count;
                    }
                    const double rmse =
                        count > 0 ? std::sqrt(sumSq / count) : std::numeric_limits<double>::quiet_NaN();
                    std::printf("  %+4.0f %+4.0f n=%lld rmse=%.2f\n", dx, dy, count, rmse);
                    if (count > 0 && rmse < bestRmse) {
                        bestRmse = rmse;
                        bestDx = dx;
                        bestDy = dy;
                        bestN = count;
                    }
                }
            }
            std::printf("advect best: dxE=%+.0fkm dyN=%+.0fkm n=%lld rmse=%.2f\n", bestDx, bestDy,
                        bestN, bestRmse);
        }
    }
    return EXIT_SUCCESS;
}
