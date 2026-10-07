// rotation_replay: run the live circulation + shear path over archived
// Archive-II volumes and emit a detection CSV for scoring.
//
// Usage:
//   rotation_replay <case-dir-with-ArchiveII-files> --lat 37.0683 --lon -88.7717 --elev 154.2
//     [--nyquist 28] [--csv] [--meso-dv 10] [--tvs-dv 25]
//
// Output CSV columns:
//   volume_time,volume_file,kind,lat,lon,delta_v_mps,diameter_km,depth_m,tilts
// where kind is TVS / MESO / ROT (ROT = thresholded shear object from
// extractRadarStormFeatures, the pink double-ring layer).
//
// This intentionally reuses the exact live pipeline: decodeLevel2Archive ->
// processVelocity(Dealiased) -> detectCirculations + extractRadarStormFeatures.
// Threshold sweeps for tuning change only the config, never the code path.

#include "app/archive_level2.hpp"
#include "core/circulation_detector.hpp"
#include "core/radar_lightning_fusion.hpp"
#include "core/shear_grid.hpp"
#include "core/velocity_processing.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: rotation_replay <dir> --lat LAT --lon LON --elev M [--nyquist N] [--csv]\n"
                 "       [--meso-dv X] [--tvs-dv Y] [--min-reflect DBZ] [--max-dv V]\n"
                 "       [--min-gate V] [--same-sign-boost B] [--single-tilt-tvs 0/1]\n"
                 "       [--min-depth-tvs M] [--min-depth-meso M] [--rot-span V]\n"
                 "       [--rot-shear S] [--rot-grid DEG] [--max-phys V] [--shear S]\n"
                 "       [--vertical-align KM] [--diagnose] [--persist 0/1]\n");
}

std::string isoTimeUtc(const QDateTime& t) {
    return t.toUTC().toString(Qt::ISODate).toStdString();
}

}  // namespace

int main(int argc, char** argv) {
    using namespace cappedweather;
    if (argc < 2) {
        usage();
        return EXIT_FAILURE;
    }
    QCoreApplication app(argc, argv);

    std::string dir = argv[1];
    double lat = std::numeric_limits<double>::quiet_NaN();
    double lon = std::numeric_limits<double>::quiet_NaN();
    double elev = 0.0;
    double nyquist = 28.0;
    double stormDir = 0.0;
    double stormSpeed = 0.0;  // >0 enables storm-relative velocity (dealised
    // first, then radial storm motion subtracted). Removes mean-flow bias so
    // fast-mover couplets (Mayfield at ~28 m/s) regain opposite signs without
    // the FAR-heavy same-sign path.
    CirculationConfig circ{};
    RadarFeatureConfig featCfg{};
    ShearGridConfig shearCfg{};
    shearCfg.minShearPerSecond = 0.0;  // disabled unless --shear is passed
    bool rotDealiased = false;  // native by default: dealiased spans hit 60-200
    // m/s on real volumes (adjacent rays unfold to different intervals), so
    // span is garbage. Native caps span at 2*Nyquist (~56) — underestimates
    // true shear when folded, but at least physical. Split-cut fix (own lowest
    // tilt per product) is what revived ROT, not dealiasing.
    VelocityProcessingOptions velPrototype;
    bool csv = false;
    bool diagnose = false;
    bool persist = false;  // track persistence across volumes (live behavior):
    // a marker must repeat within 8 km before it is emitted, so single-volume
    // dealias glints never score. Mirrors persistCirculations (2 hits to show,
    // 2 misses to drop, 24-track cap).
    for (int i = 2; i < argc;) {
        const std::string flag = argv[i];
        if (flag == "--csv") {
            csv = true;
            ++i;
            continue;
        }
        if (flag == "--diagnose") {
            diagnose = true;
            ++i;
            continue;
        }
        if (i + 1 >= argc) {
            usage();
            return EXIT_FAILURE;
        }
        const double value = std::atof(argv[i + 1]);
        if (flag == "--lat") lat = value;
        else if (flag == "--lon") lon = value;
        else if (flag == "--elev") elev = value;
        else if (flag == "--nyquist") nyquist = value;
        else if (flag == "--storm-dir") stormDir = value;
        else if (flag == "--storm-speed") stormSpeed = value;
        else if (flag == "--meso-dv") circ.mesoBaseDeltaV = value;
        else if (flag == "--tvs-dv") circ.tvsBaseDeltaV = value;
        else if (flag == "--max-dv") circ.maxDeltaVelocityMetersPerSecond = value;
        else if (flag == "--min-reflect") {
            circ.minReflectivityDbz = value;
            featCfg.minimumReflectivityDbz = value;
        } else if (flag == "--min-gate") circ.minGateVelocityMetersPerSecond = value;
        else if (flag == "--min-gate-slope") circ.minGateRangeSlope = value;
        else if (flag == "--same-sign-boost") circ.sameSignBoost = value;
        else if (flag == "--single-tilt-tvs") circ.allowSingleTiltTvs = value != 0.0;
        else if (flag == "--min-depth-tvs") circ.minDepthTvsMeters = value;
        else if (flag == "--min-depth-meso") circ.minDepthMesoMeters = value;
        else if (flag == "--vertical-align") circ.verticalAlignKm = value;
        else if (flag == "--rot-span") featCfg.minimumVelocitySpanMetersPerSecond = value;
        else if (flag == "--rot-shear") featCfg.minimumAzimuthalShearPerSecond = value;
        else if (flag == "--rot-grid") featCfg.gridSizeDeg = value;
        else if (flag == "--rot-dealiased") rotDealiased = value != 0.0;
        else if (flag == "--max-phys") velPrototype.maxPhysicalVelocityMetersPerSecond = value;
        else if (flag == "--shear") shearCfg.minShearPerSecond = value;
        else if (flag == "--persist") persist = value != 0.0;
        else {
            usage();
            return EXIT_FAILURE;
        }
        i += 2;
    }
    if (!std::isfinite(lat) || !std::isfinite(lon)) {
        std::fprintf(stderr, "rotation_replay: --lat/--lon are required\n");
        return EXIT_FAILURE;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        // Archive-II objects have no extension (KPAH20211211_000222_V06).
        if (name == "volumes.json" || name == "spc_rpts.csv" || name.ends_with(".json") ||
            name.ends_with(".csv") || name.ends_with(".geojson") || name.ends_with(".log") ||
            name.ends_with(".md")) {
            continue;
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "rotation_replay: no Archive-II files in %s\n", dir.c_str());
        return EXIT_FAILURE;
    }

    if (csv) {
        std::printf("volume_time,volume_file,kind,lat,lon,delta_v_mps,diameter_km,depth_m,tilts\n");
    }
    int volumesOk = 0;
    int volumesFailed = 0;
    int tvsCount = 0, mesoCount = 0, rotCount = 0, shearCount = 0;
    struct Track {
        CirculationDetection det;
        int hits = 0;
        int misses = 0;
        double prevLat = 0.0;  // position at previous hit (for velocity)
        double prevLon = 0.0;
        long long prevSecs = 0;
        double velLatDegPerSec = 0.0;  // storm-motion estimate, smoothed
        double velLonDegPerSec = 0.0;
        bool hasVel = false;
    };
    std::vector<Track> tracks;
    auto haversineKm = [](double la1, double lo1, double la2, double lo2) {
        // Compact haversine (replay-only; live uses rangeAndBearing).
        constexpr double kR = 6371.0, kD = 3.14159265358979323846 / 180.0;
        const double p1 = la1 * kD, p2 = la2 * kD;
        const double dp = p2 - p1, dl = (lo2 - lo1) * kD;
        const double h =
            std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) *
            std::sin(dl / 2) * std::sin(dl / 2);
        return 2 * kR * std::asin(std::sqrt(h));
    };
    for (const auto& path : files) {
        QFile file(QString::fromStdString(path.string()));
        if (!file.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "skip %s: cannot open\n", path.filename().string().c_str());
            ++volumesFailed;
            continue;
        }
        const QByteArray bytes = file.readAll();
        QString error;
        auto decoded = decodeLevel2Archive(bytes, &error, {});
        if (!decoded) {
            std::fprintf(stderr, "skip %s: %s\n", path.filename().string().c_str(),
                         error.toStdString().c_str());
            ++volumesFailed;
            continue;
        }
        ++volumesOk;
        QDateTime volTime =
            archiveTimeFromFileName(QString::fromStdString(path.filename().string()));
        if (!volTime.isValid()) {
            volTime = QDateTime::fromSecsSinceEpoch(
                static_cast<qint64>(std::filesystem::last_write_time(path).time_since_epoch().count()),
                QTimeZone::UTC);
        }
        const std::string volIso = isoTimeUtc(volTime);
        const std::string volFile = path.filename().string();

        VelocityProcessingOptions velOpts;
        velOpts.nyquistVelocityMetersPerSecond = nyquist;
        velOpts.maxPhysicalVelocityMetersPerSecond =
            velPrototype.maxPhysicalVelocityMetersPerSecond;
        if (stormSpeed > 0.0) {
            velOpts.mode = VelocityProcessingMode::StormRelative;
            velOpts.stormMotionDirectionDegrees = stormDir;
            velOpts.stormMotionSpeedMetersPerSecond = stormSpeed;
        } else {
            velOpts.mode = VelocityProcessingMode::Dealiased;
        }
        const auto dealised = processVelocity(decoded->decoded, velOpts);
        CirculationDiagnostics diag{};
        const auto dets = detectCirculations(decoded->decoded, dealised, lat, lon, elev, circ,
                                             diagnose ? &diag : nullptr);
        if (diagnose) {
            std::printf("%s %s: couplets=%d columns=%d single=%d weak=%d shallow=%d wide=%d bestDV=%.1f bestDepth=%.0f dets=%zu\n",
                        volIso.c_str(), volFile.c_str(), diag.tiltCoupletCount,
                        diag.columnCount, diag.singleRejects, diag.weakRejects,
                        diag.depthRejects, diag.diameterRejects, diag.bestDeltaV,
                        diag.bestDepthM, dets.size());
        }
        const ProcessedVelocity* rotVel = rotDealiased ? &dealised : nullptr;
        const auto feats =
            extractRadarStormFeatures(decoded->decoded, lat, lon, featCfg, rotVel);
        if (persist) {
            // Live-equivalent time continuity with storm-motion compensation.
            // Archive volumes are subsampled (~10-15 min apart) so a supercell
            // at 20 m/s moves 12-18 km between replays — past the live 8 km
            // gate. Tracks keep a smoothed velocity and match against the
            // predicted position, like an operational meso tracker.
            constexpr double kMatchKm = 8.0;
            const long long volSecs = volTime.toSecsSinceEpoch();
            std::vector<char> used(dets.size(), false);
            for (auto& track : tracks) {
                double predLat = track.det.latitudeDeg;
                double predLon = track.det.longitudeDeg;
                if (track.hasVel && track.hits >= 2) {
                    const double dt = static_cast<double>(volSecs - track.prevSecs);
                    if (dt > 0.0 && dt < 3600.0) {
                        predLat += track.velLatDegPerSec * dt;
                        predLon += track.velLonDegPerSec * dt;
                    }
                }
                double best = -1.0;
                double bestDist = 0.0;
                for (std::size_t i = 0; i < dets.size(); ++i) {
                    if (used[i]) continue;
                    const double dist = haversineKm(predLat, predLon,
                                                    dets[i].latitudeDeg, dets[i].longitudeDeg);
                    if (dist <= kMatchKm && (best < 0.0 || dist < bestDist)) {
                        best = static_cast<double>(i);
                        bestDist = dist;
                    }
                }
                if (best >= 0.0) {
                    const auto& nd = dets[static_cast<std::size_t>(best)];
                    const double dt = static_cast<double>(volSecs - track.prevSecs);
                    if (track.hits >= 1 && dt > 0.0 && dt < 3600.0) {
                        const double vLat = (nd.latitudeDeg - track.det.latitudeDeg) / dt;
                        const double vLon = (nd.longitudeDeg - track.det.longitudeDeg) / dt;
                        // Plausibility cap: 40 m/s storm motion in deg/sec.
                        const double cap = 40.0 / 111195.0;
                        if (std::abs(vLat) < cap && std::abs(vLon) < cap) {
                            if (!track.hasVel) {
                                track.velLatDegPerSec = vLat;
                                track.velLonDegPerSec = vLon;
                                track.hasVel = true;
                            } else {
                                track.velLatDegPerSec = 0.6 * track.velLatDegPerSec + 0.4 * vLat;
                                track.velLonDegPerSec = 0.6 * track.velLonDegPerSec + 0.4 * vLon;
                            }
                        }
                    }
                    track.prevLat = track.det.latitudeDeg;
                    track.prevLon = track.det.longitudeDeg;
                    track.prevSecs = volSecs;
                    track.det = nd;
                    track.hits += 1;
                    track.misses = 0;
                    used[static_cast<std::size_t>(best)] = true;
                } else {
                    track.misses += 1;
                }
            }
            std::erase_if(tracks, [](const Track& t) { return t.misses > 2; });
            for (std::size_t i = 0; i < dets.size(); ++i) {
                if (!used[i]) tracks.push_back({dets[i], 1, 0, 0.0, 0.0, volSecs, 0.0, 0.0, false});
            }
            while (tracks.size() > 24) {
                auto victim = std::min_element(
                    tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
                        if (a.hits != b.hits) return a.hits < b.hits;
                        return a.misses > b.misses;
                    });
                tracks.erase(victim);
            }
            for (const auto& track : tracks) {
                if (track.hits < 2) continue;
                const auto& d = track.det;
                const char* kind =
                    d.kind == CirculationKind::TornadoVortex ? "TVS" : "MESO";
                if (d.kind == CirculationKind::TornadoVortex) ++tvsCount;
                else ++mesoCount;
                std::printf("%s,%s,%s,%.5f,%.5f,%.2f,%.3f,%.0f,%d\n", volIso.c_str(),
                            volFile.c_str(), kind, d.latitudeDeg, d.longitudeDeg,
                            d.maxDeltaVelocityMetersPerSecond, d.diameterKm, d.depthMeters,
                            d.tiltCount);
            }
        } else
            for (const auto& d : dets) {
                const char* kind =
                    d.kind == CirculationKind::TornadoVortex ? "TVS" : "MESO";
                if (d.kind == CirculationKind::TornadoVortex) ++tvsCount;
                else ++mesoCount;
                std::printf("%s,%s,%s,%.5f,%.5f,%.2f,%.3f,%.0f,%d\n", volIso.c_str(),
                            volFile.c_str(), kind, d.latitudeDeg, d.longitudeDeg,
                            d.maxDeltaVelocityMetersPerSecond, d.diameterKm, d.depthMeters,
                            d.tiltCount);
            }
        for (const auto& f : feats) {
            if (!f.rotating) continue;
            ++rotCount;
            std::printf("%s,%s,ROT,%.5f,%.5f,%.2f,%.3f,,%d\n", volIso.c_str(),
                        volFile.c_str(), f.latitudeDeg, f.longitudeDeg,
                        f.velocitySpanMetersPerSecond,
                        std::sqrt(std::max(1.0, f.areaKm2)), f.reflectivityCellCount);
        }
        if (shearCfg.minShearPerSecond > 0.0) {
            shearCfg.minReflectivityDbz = circ.minReflectivityDbz;
            const auto peaks = detectShearPeaks(decoded->decoded, dealised, lat, lon,
                                               elev, shearCfg);
            for (const auto& p : peaks) {
                ++shearCount;
                std::printf("%s,%s,SHEAR,%.5f,%.5f,%.2f,%.3f,%.0f,%d\n", volIso.c_str(),
                            volFile.c_str(), p.latitudeDeg, p.longitudeDeg,
                            p.equivalentDeltaVMps, p.diameterKm, p.depthMeters,
                            p.tiltCount);
            }
            if (!csv) {
                std::printf("%s %s: rays=%zu tvs/meso=%zu rot=%zu shear=%zu\n", volIso.c_str(),
                            volFile.c_str(), decoded->decoded.rays.size(), dets.size(),
                            std::count_if(feats.begin(), feats.end(),
                                          [](const auto& f) { return f.rotating; }),
                            peaks.size());
            }
        } else if (!csv) {
            std::printf("%s %s: rays=%zu tvs/meso=%zu rot=%zu\n", volIso.c_str(),
                        volFile.c_str(), decoded->decoded.rays.size(), dets.size(),
                        std::count_if(feats.begin(), feats.end(),
                                      [](const auto& f) { return f.rotating; }));
        }
    }
    std::fprintf(stderr,
                 "\n=== rotation replay summary ===\nvolumes: %d ok, %d failed\n"
                 "detections: TVS=%d MESO=%d ROT=%d SHEAR=%d\n",
                 volumesOk, volumesFailed, tvsCount, mesoCount, rotCount, shearCount);
    return EXIT_SUCCESS;
}
