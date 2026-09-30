#include "core/storm_tracker.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace cappedweather {
namespace {

constexpr double kMaxStormSpeedMetersPerSec = 35.0;
constexpr double kMinGateMeters = 8000.0;
constexpr double kMaxGateMeters = 60000.0;
// Assignment economics (all in pseudo-km): a real match must beat coasting
// a track or birthing a new one. Birth is pricier than one coast so a
// flickering cell prefers to coast rather than fork every volume.
constexpr double kCoastBaseCost = 12.0;
constexpr double kCoastPerMissCost = 6.0;
constexpr double kBirthCost = 18.0;
constexpr double kInfCost = 1e9;

double gateFor(double dtSeconds, int misses) {
    const double dt = std::max(60.0, dtSeconds);
    double gate = std::clamp(kMaxStormSpeedMetersPerSec * dt, kMinGateMeters, kMaxGateMeters);
    // Coasted tracks widen honestly: uncertainty grows while unseen.
    gate += static_cast<double>(std::max(0, misses)) * 5000.0;
    return std::min(gate, kMaxGateMeters * 1.5);
}

// Hungarian (Munkres) for a square cost matrix. Returns col per row (-1 dummy).
// O(n^3), n = tracks+features which stays tiny (<= ~40).
std::vector<int> hungarian(const std::vector<std::vector<double>>& cost) {
    const int n = static_cast<int>(cost.size());
    if (n == 0) return {};
    std::vector<double> u(n + 1, 0.0), v(n + 1, 0.0);
    std::vector<int> p(n + 1, 0), way(n + 1, 0);
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(n + 1, kInfCost);
        std::vector<char> used(n + 1, false);
        do {
            used[j0] = true;
            const int i0 = p[j0];
            double delta = kInfCost;
            int j1 = 0;
            for (int j = 1; j <= n; ++j) {
                if (used[j]) continue;
                const double cur = cost[static_cast<std::size_t>(i0 - 1)][static_cast<std::size_t>(j - 1)] -
                                   u[static_cast<std::size_t>(i0)] - v[static_cast<std::size_t>(j)];
                if (cur < minv[static_cast<std::size_t>(j)]) {
                    minv[static_cast<std::size_t>(j)] = cur;
                    way[static_cast<std::size_t>(j)] = j0;
                }
                if (minv[static_cast<std::size_t>(j)] < delta) {
                    delta = minv[static_cast<std::size_t>(j)];
                    j1 = j;
                }
            }
            for (int j = 0; j <= n; ++j) {
                if (used[j]) {
                    u[static_cast<std::size_t>(p[j])] += delta;
                    v[static_cast<std::size_t>(j)] -= delta;
                } else {
                    minv[static_cast<std::size_t>(j)] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }
    std::vector<int> rowToCol(static_cast<std::size_t>(n), -1);
    for (int j = 1; j <= n; ++j) {
        if (p[j] > 0) rowToCol[static_cast<std::size_t>(p[j] - 1)] = j - 1;
    }
    return rowToCol;
}

}  // namespace

StormTracker::StormTracker(int maxTracks, int maxMisses)
    : maxTracks_(std::max(1, maxTracks)), maxMisses_(std::max(0, maxMisses)) {}

void StormTracker::clear() {
    tracks_.clear();
    haveTime_ = false;
    lastEpochSeconds_ = 0.0;
    nextId_ = 1;
}

void StormTracker::predictPosition(const StormTrack& track, const double epochSeconds, double& latOut,
                                   double& lonOut) {
    if (track.fixes.empty()) {
        latOut = 0.0;
        lonOut = 0.0;
        return;
    }
    const auto& tail = track.fixes.back();
    if (track.fixes.size() < 2) {
        latOut = tail.latitudeDeg;
        lonOut = tail.longitudeDeg;
        return;
    }
    const double dt = epochSeconds - tail.epochSeconds;
    latOut = tail.latitudeDeg + track.velocityLatDegPerSec * dt;
    lonOut = tail.longitudeDeg + track.velocityLonDegPerSec * dt;
}

const std::vector<StormTrack>& StormTracker::update(
    const std::vector<RadarStormFeature>& features, const double epochSeconds) {
    if (!(epochSeconds > 0.0)) return tracks_;
    // Scrub guard: never fold an older (or equal) volume back in.
    if (haveTime_ && epochSeconds <= lastEpochSeconds_) return tracks_;
    haveTime_ = true;
    lastEpochSeconds_ = epochSeconds;

    const int trackCount = static_cast<int>(tracks_.size());
    const int featureCount = static_cast<int>(features.size());

    // Order new-track candidates strongest-first for deterministic birth.
    std::vector<int> featureOrder(static_cast<std::size_t>(featureCount));
    for (int i = 0; i < featureCount; ++i) featureOrder[static_cast<std::size_t>(i)] = i;
    std::sort(featureOrder.begin(), featureOrder.end(), [&features](int a, int b) {
        return features[static_cast<std::size_t>(a)].maximumReflectivityDbz >
               features[static_cast<std::size_t>(b)].maximumReflectivityDbz;
    });

    if (trackCount == 0) {
        std::vector<StormTrack> born;
        for (const int fi : featureOrder) {
            if (static_cast<int>(born.size()) >= maxTracks_) break;
            const auto& feature = features[static_cast<std::size_t>(fi)];
            StormTrack track;
            track.id = nextId_++;
            track.fixes.push_back(StormTrackFix{feature.latitudeDeg, feature.longitudeDeg,
                                                feature.maximumReflectivityDbz,
                                                feature.azimuthalShearPerSecond, feature.rotating,
                                                epochSeconds});
            track.hits = 1;
            track.ageVolumes = 1;
            born.push_back(std::move(track));
        }
        tracks_ = std::move(born);
        return tracks_;
    }

    // Predict + gate + cost.
    std::vector<double> predLat(static_cast<std::size_t>(trackCount));
    std::vector<double> predLon(static_cast<std::size_t>(trackCount));
    std::vector<double> gates(static_cast<std::size_t>(trackCount));
    for (int ti = 0; ti < trackCount; ++ti) {
        const auto& track = tracks_[static_cast<std::size_t>(ti)];
        predictPosition(track, epochSeconds, predLat[static_cast<std::size_t>(ti)],
                        predLon[static_cast<std::size_t>(ti)]);
        const double dt = track.fixes.empty()
                              ? 60.0
                              : std::max(60.0, epochSeconds - track.fixes.back().epochSeconds);
        gates[static_cast<std::size_t>(ti)] = gateFor(dt, track.misses);
    }

    // Square (T+F) matrix. Columns 0..F-1 are real features, F..F+T-1 are
    // per-track coast slots (exactly one usable per track row). Rows 0..T-1
    // are tracks, T..T+F-1 are birth slots.
    const int n = trackCount + featureCount;
    std::vector<std::vector<double>> cost(static_cast<std::size_t>(n),
                                          std::vector<double>(static_cast<std::size_t>(n), kInfCost));
    for (int ti = 0; ti < trackCount; ++ti) {
        const auto& track = tracks_[static_cast<std::size_t>(ti)];
        const auto& tail = track.fixes.back();
        for (int fi = 0; fi < featureCount; ++fi) {
            const auto& feature = features[static_cast<std::size_t>(fi)];
            const double dist = haversineMeters(predLat[static_cast<std::size_t>(ti)],
                                                predLon[static_cast<std::size_t>(ti)],
                                                feature.latitudeDeg, feature.longitudeDeg);
            if (dist > gates[static_cast<std::size_t>(ti)]) {
                cost[static_cast<std::size_t>(ti)][static_cast<std::size_t>(fi)] = kInfCost;
                continue;
            }
            const double dDbz =
                std::abs(feature.maximumReflectivityDbz - tail.maximumReflectivityDbz);
            const double areaPenalty = 0.0;  // tail carries no area; dBZ term dominates
            // dBZ penalty in pseudo-km: 1.5 km per dB, capped so a hail core
            // jump doesn't veto an otherwise perfect motion match.
            const double dbzPenalty = std::min(20.0, 1.5 * dDbz);
            // Rotation agreement: a rotating track holding rotation is cheaper.
            double rotBonus = 0.0;
            if (tail.rotating && feature.rotating) rotBonus = -3.0;
            cost[static_cast<std::size_t>(ti)][static_cast<std::size_t>(fi)] =
                dist / 1000.0 + dbzPenalty + areaPenalty + rotBonus;
        }
        // Exactly one coast slot per track row; the rest stay INF.
        cost[static_cast<std::size_t>(ti)][static_cast<std::size_t>(featureCount + ti)] =
            kCoastBaseCost + kCoastPerMissCost * track.misses;
    }
    // Birth slots take any feature at birth cost; slot-vs-slot is free.
    for (int k = 0; k < featureCount; ++k) {
        for (int fi = 0; fi < featureCount; ++fi) {
            cost[static_cast<std::size_t>(trackCount + k)][static_cast<std::size_t>(fi)] = kBirthCost;
        }
        for (int j = featureCount; j < n; ++j) {
            cost[static_cast<std::size_t>(trackCount + k)][static_cast<std::size_t>(j)] = 0.0;
        }
    }

    const auto assignment = hungarian(cost);

    std::vector<int> trackMatch(trackCount, -1);
    std::vector<char> featureTaken(static_cast<std::size_t>(featureCount), false);
    for (int ti = 0; ti < trackCount; ++ti) {
        const int col = assignment[static_cast<std::size_t>(ti)];
        if (col >= 0 && col < featureCount &&
            cost[static_cast<std::size_t>(ti)][static_cast<std::size_t>(col)] < kInfCost / 2) {
            trackMatch[static_cast<std::size_t>(ti)] = col;
            featureTaken[static_cast<std::size_t>(col)] = true;
        }
    }

    // Snapshot pre-move state for split-parent lookup (matched rows are moved
    // below, so ids/predictions must be captured before any std::move).
    struct TrackSnapshot {
        int id = 0;
        double predLat = 0.0;
        double predLon = 0.0;
        double gate = 0.0;
    };
    std::vector<TrackSnapshot> snapshots;
    snapshots.reserve(static_cast<std::size_t>(trackCount));
    for (int ti = 0; ti < trackCount; ++ti) {
        snapshots.push_back(TrackSnapshot{tracks_[static_cast<std::size_t>(ti)].id,
                                          predLat[static_cast<std::size_t>(ti)],
                                          predLon[static_cast<std::size_t>(ti)],
                                          gates[static_cast<std::size_t>(ti)]});
    }

    std::vector<StormTrack> next;
    next.reserve(static_cast<std::size_t>(trackCount + featureCount));

    // Matched continuations (track-id order for determinism).
    for (int ti = 0; ti < trackCount; ++ti) {
        const int fi = trackMatch[static_cast<std::size_t>(ti)];
        if (fi < 0) continue;
        const auto& feature = features[static_cast<std::size_t>(fi)];
        StormTrack continued = std::move(tracks_[static_cast<std::size_t>(ti)]);
        const auto& tail = continued.fixes.back();
        const double dt = std::max(60.0, epochSeconds - tail.epochSeconds);
        // Velocity update from newest displacement, smoothed against history.
        if (!continued.fixes.empty() && dt > 0.0) {
            const double newVLat = (feature.latitudeDeg - tail.latitudeDeg) / dt;
            const double newVLon = (feature.longitudeDeg - tail.longitudeDeg) / dt;
            if (continued.fixes.size() < 2) {
                continued.velocityLatDegPerSec = newVLat;
                continued.velocityLonDegPerSec = newVLon;
            } else {
                continued.velocityLatDegPerSec =
                    0.6 * newVLat + 0.4 * continued.velocityLatDegPerSec;
                continued.velocityLonDegPerSec =
                    0.6 * newVLon + 0.4 * continued.velocityLonDegPerSec;
            }
            // Clamp to physical storm motion so one bad match can't arm a
            // cross-state arrow on the next prediction.
            const double vMag = std::hypot(continued.velocityLatDegPerSec * 111000.0,
                                           continued.velocityLonDegPerSec * 111000.0);
            if (vMag > 60.0) {
                const double scale = 60.0 / vMag;
                continued.velocityLatDegPerSec *= scale;
                continued.velocityLonDegPerSec *= scale;
            }
        }
        continued.fixes.push_back(StormTrackFix{feature.latitudeDeg, feature.longitudeDeg,
                                                feature.maximumReflectivityDbz,
                                                feature.azimuthalShearPerSecond, feature.rotating,
                                                epochSeconds});
        while (continued.fixes.size() > 64) continued.fixes.pop_front();
        continued.misses = 0;
        continued.hits += 1;
        continued.ageVolumes += 1;
        next.push_back(std::move(continued));
    }

    // Births for unmatched features, strongest first (covers splits: the
    // divergent piece starts its own track with parentId set when it falls
    // inside a nearby track's 2x gate).
    for (const int fi : featureOrder) {
        if (featureTaken[static_cast<std::size_t>(fi)]) continue;
        if (static_cast<int>(next.size()) >= maxTracks_) break;
        const auto& feature = features[static_cast<std::size_t>(fi)];
        StormTrack born;
        born.id = nextId_++;
        born.fixes.push_back(StormTrackFix{feature.latitudeDeg, feature.longitudeDeg,
                                           feature.maximumReflectivityDbz,
                                           feature.azimuthalShearPerSecond, feature.rotating,
                                           epochSeconds});
        born.hits = 1;
        born.ageVolumes = 1;
        // Split parent: nearest snapshot whose 2x gate contains the birth.
        int parent = 0;
        double bestDist = 0.0;
        for (const auto& snap : snapshots) {
            const double dist = haversineMeters(snap.predLat, snap.predLon, feature.latitudeDeg,
                                                feature.longitudeDeg);
            const double wide = snap.gate * 2.0;
            if (dist <= wide && (parent == 0 || dist < bestDist)) {
                parent = snap.id;
                bestDist = dist;
            }
        }
        born.parentId = parent;
        next.push_back(std::move(born));
    }

    // Coast unmatched tracks briefly; threshold flicker, not dissipation.
    // tracks_ entries for matched rows are moved-from; guard via trackMatch.
    for (int ti = 0; ti < trackCount; ++ti) {
        if (trackMatch[static_cast<std::size_t>(ti)] >= 0) continue;
        StormTrack coasted = std::move(tracks_[static_cast<std::size_t>(ti)]);
        if (coasted.fixes.empty()) continue;
        if (++coasted.misses <= maxMisses_ && static_cast<int>(next.size()) < maxTracks_) {
            coasted.ageVolumes += 1;
            next.push_back(std::move(coasted));
        }
    }

    // Deterministic order: by id so UI + tests don't depend on match order.
    std::sort(next.begin(), next.end(),
              [](const StormTrack& a, const StormTrack& b) { return a.id < b.id; });
    // Enforce cap after coasting: drop shortest-lived coasted first.
    while (static_cast<int>(next.size()) > maxTracks_) {
        auto victim = std::min_element(next.begin(), next.end(), [](const StormTrack& a, const StormTrack& b) {
            if (a.fixes.size() != b.fixes.size()) return a.fixes.size() < b.fixes.size();
            return a.hits < b.hits;
        });
        next.erase(victim);
    }
    tracks_ = std::move(next);
    return tracks_;
}

}  // namespace cappedweather
