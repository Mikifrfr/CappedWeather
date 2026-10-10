#pragma once

// Hail swaths: per-storm accumulation of VIL-density hail cores along
// StormTracker paths. Each fusion completion matches the volume's hail cores
// to the nearest storm track (predicted position, 15 km gate); matches
// append a (lat, lon, tier, epoch) fix to that storm's swath. The swath
// polygon is the convex hull of its fixes (3+ distinct points): a guidance
// footprint of where severe hail has repeatedly verified inside one storm,
// not a forecast and never a diameter.
//
// Swaths export as KML Placemark polygons alongside the frame GroundOverlay
// (see MainWindow::exportFrameKml). Pure core types, no Qt, deterministic.

#include "core/hail_detector.hpp"
#include "core/storm_tracker.hpp"

#include <utility>
#include <vector>

namespace cappedweather {

struct HailSwathFix {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    int tier = 0;
    double epochSeconds = 0.0;
};

struct HailSwath {
    int trackId = 0;
    std::vector<HailSwathFix> fixes;
    int maxTier = 0;
};

struct HailSwathConfig {
    double matchKm = 15.0;  // core-to-track gate (storm scale + motion miss)
    std::size_t maxFixesPerSwath = 128;  // oldest fixes drop first
};

// Convex hull (Andrew monotone chain) of the swath fixes as a closed
// (lat, lon) ring. Empty when fewer than 3 distinct points exist: a line
// is a path, not a footprint.
std::vector<std::pair<double, double>> hailSwathHull(const HailSwath& swath);

class HailSwathTracker {
public:
    explicit HailSwathTracker(HailSwathConfig config = {});

    // Fold one volume's hail cores in against the tracker's live tracks.
    // Non-finite epochs and empty inputs are ignored (fail closed).
    void update(const std::vector<StormTrack>& tracks, const std::vector<HailCore>& cores,
                double epochSeconds);
    const std::vector<HailSwath>& swaths() const { return swaths_; }
    void clear();

private:
    HailSwathConfig config_;
    std::vector<HailSwath> swaths_;
};

}  // namespace cappedweather
