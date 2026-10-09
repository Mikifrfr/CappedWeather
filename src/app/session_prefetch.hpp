#pragma once

// Field-reliability prep: prefetch pick-list for spilled session volumes.
// No threading here by design. MainWindow::scheduleSpillPrefetch() consumes
// the pick list below (QThreadPool job faulting spilled volumes back in via
// loadSpilledVolume(), published with installPrefetchedChunk()).
// CrossSectionWidget and the filmstrip scrub path fault in synchronously via
// sessionFrameChunk(), so warming the ±1/±2 neighbours ahead of a scrub
// step is what removes the hitch on field hardware.

#include "app/session_store.hpp"

#include <QStringList>

#include <cstddef>
#include <vector>

namespace cappedweather {

// Pure, unit-testable neighbour ordering around the current frame.
//
// historySize: session.history.size().
// current:     index of the currently displayed frame (-1 = no selection).
// count:       maximum neighbours to return (clamped to the 4 available).
//
// Order is forward-biased proximity: +1, -1, +2, -2. Forward first because
// followLatest/playback and timeline "next" move forward far more often than
// backward; a background warmer should fault them in this order and stop at
// the first resident/missing frame only if it wants strict priority.
// Out-of-range neighbours are skipped, never clamped, so the result never
// contains `current` itself and never exceeds the history bounds.
// Empty when historySize == 0, current is not a live index, or count <= 0.
inline std::vector<int> nextSpillIndexes(std::size_t historySize, int current, int count = 4) {
    std::vector<int> out;
    if (historySize == 0 || count <= 0) return out;
    if (current < 0 || current >= static_cast<int>(historySize)) return out;
    // Fixed neighbour offsets, closest first with a forward bias.
    const int kOffsets[] = {+1, -1, +2, -2};
    for (const int offset : kOffsets) {
        if (static_cast<int>(out.size()) >= count) break;
        const int candidate = current + offset;
        if (candidate < 0 || candidate >= static_cast<int>(historySize)) continue;
        out.push_back(candidate);
    }
    return out;
}

// Thin adapter over RadarSession: maps the pure index list onto spilled
// file paths, keeping only frames worth prefetching (evicted to disk and
// not yet faulted back in: empty chunk + non-empty spilledPath, per the
// session_store invariant). Resident frames (chunk set) and dropped frames
// (both empty) are skipped so a warmer never re-reads RAM or retries
// volumes already discarded by enforceSessionSpillBudget().
class SessionPrefetcher {
public:
    // Indexes of neighbours that are currently spilled to disk, in
    // nextSpillIndexes() order. No I/O, no threading.
    static inline std::vector<int> pickSpillIndexes(const RadarSession& session, int current,
                                                    int count = 4) {
        std::vector<int> out;
        const auto candidates = nextSpillIndexes(session.history.size(), current, count);
        for (const int index : candidates) {
            const auto& frame = session.history.at(static_cast<std::size_t>(index));
            if (!frame.chunk && !frame.spilledPath.isEmpty()) out.push_back(index);
        }
        return out;
    }

    // Convenience overload using the session's live selection.
    // Single-arg on purpose: keeps pickSpillIndexes(session, current, count)
    // unambiguous for two-arg calls (those always mean explicit current).
    static inline std::vector<int> pickSpillIndexes(const RadarSession& session) {
        return pickSpillIndexes(session, session.frameIndex, 4);
    }

    // Spilled file paths to prefetch, same order as pickSpillIndexes().
    // The warmer faults each path via loadSpilledVolume() off the UI
    // thread and then publishes through installPrefetchedChunk(); it must respect
    // the spill budget and never exceed kSpilledKeepVolumes + RAM window.
    static inline QStringList pickSpillPaths(const RadarSession& session, int current,
                                             int count = 4) {
        QStringList out;
        for (const int index : pickSpillIndexes(session, current, count))
            out.push_back(session.history.at(static_cast<std::size_t>(index)).spilledPath);
        return out;
    }

    // Convenience overload using the session's live selection (see note
    // above: single-arg so two-arg calls unambiguously mean explicit current).
    static inline QStringList pickSpillPaths(const RadarSession& session) {
        return pickSpillPaths(session, session.frameIndex, 4);
    }
};

}  // namespace cappedweather
