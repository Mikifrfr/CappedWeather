#pragma once

#include "core/radar_lightning_fusion.hpp"

#include <cstddef>
#include <deque>
#include <vector>

namespace cappedweather {

// Per-storm tracks across volumes. The old single-centroid trail followed
// the global maximum, so the max jumping between cells read as absurd
// motion. The tracker instead keeps one history per storm, matched by
// proximity between volumes, with short coasting when a volume misses.
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
};

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

private:
    std::vector<StormTrack> tracks_;
    double lastEpochSeconds_ = 0.0;
    bool haveTime_ = false;
    int nextId_ = 1;
    int maxTracks_ = 12;
    int maxMisses_ = 2;
};

} // namespace cappedweather
