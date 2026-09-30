#include "core/storm_tracker.hpp"

#include "core/radar_geometry.hpp"

#include <algorithm>

namespace cappedweather {

StormTracker::StormTracker(int maxTracks, int maxMisses)
    : maxTracks_(std::max(1, maxTracks)), maxMisses_(std::max(0, maxMisses)) {}

void StormTracker::clear() {
    tracks_.clear();
    haveTime_ = false;
    lastEpochSeconds_ = 0.0;
    nextId_ = 1;
}

const std::vector<StormTrack>& StormTracker::update(
    const std::vector<RadarStormFeature>& features, const double epochSeconds) {
    if (!(epochSeconds > 0.0)) return tracks_;
    // Scrub guard: never fold an older (or equal) volume back in.
    if (haveTime_ && epochSeconds <= lastEpochSeconds_) return tracks_;
    haveTime_ = true;
    lastEpochSeconds_ = epochSeconds;

    // Strongest first so the dominant storm claims its continuation.
    std::vector<const RadarStormFeature*> ordered;
    ordered.reserve(features.size());
    for (const auto& feature : features) ordered.push_back(&feature);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->maximumReflectivityDbz > right->maximumReflectivityDbz;
    });

    std::vector<bool> claimed(tracks_.size(), false);
    std::vector<StormTrack> next;
    next.reserve(tracks_.size() + ordered.size());
    for (const auto* feature : ordered) {
        // Gate: a storm plausibly moves under ~35 m/s; below ~8 km is
        // centroid wobble worth absorbing, above the cap is a different
        // storm. dt comes from the track tail so coasted tracks widen
        // their gate honestly.
        int best = -1;
        double bestDistance = 0.0;
        for (std::size_t i = 0; i < tracks_.size(); ++i) {
            if (claimed[i] || tracks_[i].fixes.empty()) continue;
            const auto& tail = tracks_[i].fixes.back();
            const auto dt = std::max(60.0, epochSeconds - tail.epochSeconds);
            const auto gate = std::clamp(35000.0 * dt, 8000.0, 60000.0);
            const auto distance = haversineMeters(tail.latitudeDeg, tail.longitudeDeg,
                                                  feature->latitudeDeg, feature->longitudeDeg);
            if (distance <= gate && (best < 0 || distance < bestDistance)) {
                best = static_cast<int>(i);
                bestDistance = distance;
            }
        }
        StormTrackFix fix{feature->latitudeDeg,
                          feature->longitudeDeg,
                          feature->maximumReflectivityDbz,
                          feature->azimuthalShearPerSecond,
                          feature->rotating,
                          epochSeconds};
        if (best >= 0) {
            claimed[static_cast<std::size_t>(best)] = true;
            StormTrack continued = std::move(tracks_[static_cast<std::size_t>(best)]);
            continued.fixes.push_back(fix);
            // Bound per-track history: long sessions would otherwise grow the
            // deque without limit. 64 fixes is hours of volumes at WSR-88D cadence.
            while (continued.fixes.size() > 64) continued.fixes.pop_front();
            continued.misses = 0;
            next.push_back(std::move(continued));
        } else if (static_cast<int>(next.size()) < maxTracks_) {
            // New storm (also covers splits: the divergent piece starts its
            // own track rather than yanking the parent's history sideways).
            StormTrack born;
            born.id = nextId_++;
            born.fixes.push_back(fix);
            next.push_back(std::move(born));
        }
    }
    // Coast unmatched tracks briefly; a volume or two without a match is
    // usually threshold flicker, not dissipation.
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        if (claimed[i]) continue;
        StormTrack coasted = std::move(tracks_[i]);
        if (++coasted.misses <= maxMisses_ && static_cast<int>(next.size()) < maxTracks_)
            next.push_back(std::move(coasted));
    }
    tracks_ = std::move(next);
    return tracks_;
}

} // namespace cappedweather
