#pragma once

#include "core/radar_lightning_fusion.hpp"

#include <cstddef>
#include <deque>
#include <limits>
#include <unordered_map>
#include <vector>

namespace cappedweather {

// Per-storm tracks across volumes. Cost-based assignment with motion
// prediction: each track predicts its position at the new epoch from its
// estimated velocity, candidates are gated around the prediction, and a
// global Hungarian assignment minimizes distance + intensity/area penalties.
// Splits seed a new track carrying the parent id; merges coast the loser.
// Timeline scrubbing (older-or-equal epochs) is ignored so it can never
// corrupt the tracks.
struct StormTrackFix {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double maximumReflectivityDbz = 0.0;
    double azimuthalShearPerSecond = 0.0;
    bool rotating = false;
    double epochSeconds = 0.0;
};

struct StormTrack {
    int id = 0;
    std::deque<StormTrackFix> fixes;
    int misses = 0;
    // Motion state (deg/sec, estimated from recent fixes, smoothed).
    double velocityLatDegPerSec = 0.0;
    double velocityLonDegPerSec = 0.0;
    // Split origin: 0 when born standalone, else the parent track id.
    int parentId = 0;
    // Lifetime stats for quality/confidence display.
    int hits = 0;
    int ageVolumes = 0;
};

// Per-track running maximum reflectivity location ("max trail").
// Header-only helper for analysis depth: keeps the strongest gate seen per
// track id so trails survive centroid wobble. Pure core type, no Qt.
struct GateMaxTrail {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    double maxDbz = -std::numeric_limits<double>::infinity();
    double epochSeconds = 0.0;
};

// Keeps the running max for one track id. Inserts on first sight; otherwise
// replaces only when dbz strictly exceeds the stored max. Non-finite dbz or
// non-positive epochs are ignored. Returns true when the trail was created
// or improved.
inline bool updateGateMaxTrail(std::unordered_map<int, GateMaxTrail>& trails, int trackId,
                               double latitudeDeg, double longitudeDeg, double maxDbz,
                               double epochSeconds) {
    if (!(maxDbz == maxDbz) || !(epochSeconds > 0.0)) return false;
    const auto it = trails.find(trackId);
    if (it == trails.end()) {
        trails.emplace(trackId, GateMaxTrail{latitudeDeg, longitudeDeg, maxDbz, epochSeconds});
        return true;
    }
    if (maxDbz > it->second.maxDbz) {
        it->second = GateMaxTrail{latitudeDeg, longitudeDeg, maxDbz, epochSeconds};
        return true;
    }
    return false;
}

// Fix-shaped convenience overload (unit-test hook).
inline bool updateGateMaxTrail(std::unordered_map<int, GateMaxTrail>& trails, int trackId,
                               const StormTrackFix& fix) {
    return updateGateMaxTrail(trails, trackId, fix.latitudeDeg, fix.longitudeDeg,
                              fix.maximumReflectivityDbz, fix.epochSeconds);
}

// Fold a whole track's fixes into the trail map; keeps the per-id maxima.
inline void updateGateMaxTrails(std::unordered_map<int, GateMaxTrail>& trails,
                                const StormTrack& track) {
    for (const auto& fix : track.fixes) updateGateMaxTrail(trails, track.id, fix);
}

class StormTracker {
public:
    StormTracker(int maxTracks = 12, int maxMisses = 2);

    // Fold one volume's features in. Updates with epochSeconds at or before
    // the newest tracked fix are ignored, so timeline scrubbing can never
    // corrupt the tracks. Returns the live tracks.
    const std::vector<StormTrack>& update(const std::vector<RadarStormFeature>& features,
                                          double epochSeconds);
    const std::vector<StormTrack>& tracks() const { return tracks_; }
    void clear();

    // Predicted position of a track at a future (or present) epoch from its
    // estimated velocity. Falls back to the tail fix when < 2 fixes.
    static void predictPosition(const StormTrack& track, double epochSeconds, double& latOut,
                                double& lonOut);

private:
    std::vector<StormTrack> tracks_;
    double lastEpochSeconds_ = 0.0;
    bool haveTime_ = false;
    int nextId_ = 1;
    int maxTracks_ = 12;
    int maxMisses_ = 2;
};

} // namespace cappedweather
