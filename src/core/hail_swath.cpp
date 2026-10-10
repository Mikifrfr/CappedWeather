#include "core/hail_swath.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace cappedweather {

namespace {

struct HullPoint {
    double lon = 0.0;
    double lat = 0.0;
};

double cross(const HullPoint& o, const HullPoint& a, const HullPoint& b) {
    return (a.lon - o.lon) * (b.lat - o.lat) - (a.lat - o.lat) * (b.lon - o.lon);
}

}  // namespace

std::vector<std::pair<double, double>> hailSwathHull(const HailSwath& swath) {
    std::vector<HullPoint> points;
    for (const auto& fix : swath.fixes) {
        if (!std::isfinite(fix.latitudeDeg) || !std::isfinite(fix.longitudeDeg)) continue;
        points.push_back({fix.longitudeDeg, fix.latitudeDeg});
    }
    std::sort(points.begin(), points.end(), [](const HullPoint& a, const HullPoint& b) {
        if (a.lon != b.lon) return a.lon < b.lon;
        return a.lat < b.lat;
    });
    points.erase(std::unique(points.begin(), points.end(),
                             [](const HullPoint& a, const HullPoint& b) {
                                 return a.lon == b.lon && a.lat == b.lat;
                             }),
                 points.end());
    if (points.size() < 3) return {};
    std::vector<HullPoint> hull;
    for (const auto& point : points) {
        while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull[hull.size() - 1], point) <= 0.0)
            hull.pop_back();
        hull.push_back(point);
    }
    const std::size_t lower = hull.size();
    for (std::size_t i = points.size(); i-- > 0;) {
        while (hull.size() > lower &&
               cross(hull[hull.size() - 2], hull[hull.size() - 1], points[i]) <= 0.0)
            hull.pop_back();
        hull.push_back(points[i]);
    }
    hull.pop_back();  // first == last
    if (hull.size() < 3) return {};
    std::vector<std::pair<double, double>> ring;
    for (const auto& point : hull) ring.emplace_back(point.lat, point.lon);
    return ring;
}

HailSwathTracker::HailSwathTracker(HailSwathConfig config) : config_(config) {}

void HailSwathTracker::update(const std::vector<StormTrack>& tracks,
                              const std::vector<HailCore>& cores, const double epochSeconds) {
    if (!std::isfinite(epochSeconds) || epochSeconds <= 0.0) return;
    if (tracks.empty() || cores.empty()) return;
    for (const auto& core : cores) {
        if (!std::isfinite(core.latitudeDeg) || !std::isfinite(core.longitudeDeg)) continue;
        if (core.tier <= 0) continue;
        const StormTrack* best = nullptr;
        double bestDist = 0.0;
        for (const auto& track : tracks) {
            if (track.fixes.empty()) continue;
            double predLat = 0.0, predLon = 0.0;
            StormTracker::predictPosition(track, epochSeconds, predLat, predLon);
            if (!std::isfinite(predLat) || !std::isfinite(predLon)) continue;
            const double dist =
                haversineMeters(predLat, predLon, core.latitudeDeg, core.longitudeDeg);
            if (!std::isfinite(dist)) continue;
            if (dist / 1000.0 <= config_.matchKm && (!best || dist < bestDist)) {
                best = &track;
                bestDist = dist;
            }
        }
        if (!best) continue;
        auto it = std::find_if(swaths_.begin(), swaths_.end(),
                               [id = best->id](const HailSwath& swath) { return swath.trackId == id; });
        if (it == swaths_.end()) {
            swaths_.push_back({best->id, {}, 0});
            it = swaths_.end() - 1;
        }
        it->fixes.push_back({core.latitudeDeg, core.longitudeDeg, core.tier, epochSeconds});
        it->maxTier = std::max(it->maxTier, core.tier);
        while (it->fixes.size() > config_.maxFixesPerSwath)
            it->fixes.erase(it->fixes.begin());
    }
}

void HailSwathTracker::clear() {
    swaths_.clear();
}

}  // namespace cappedweather
