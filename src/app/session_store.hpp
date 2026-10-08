#pragma once

// Extracted from MainWindow: per-radar volume history plus spill-to-disk.
// MainWindow keeps playback/UI; this module owns the data window so session
// logic is unit-testable without a QMainWindow.

#include "core/level2_decoder.hpp"

#include <QDateTime>
#include <QSet>
#include <QString>

#include <cstddef>
#include <limits>
#include <memory>
#include <vector>

namespace cappedweather {

struct RadarFrame {
    QString volumeId;
    std::shared_ptr<DecodedChunk> chunk;
    QDateTime dataTime;
    int chunkCount = 0;
    int failedChunks = 0;
    // Spill-to-disk: empty chunk + non-empty path means the frame lives
    // on disk and faults back in when scrubbed to. Invariant: exactly
    // one of chunk / spilledPath is set for a live frame.
    QString spilledPath;
    // MSL height (m) the frame's CAPPI field was derived at. NaN when the
    // frame carries no CAPPI field. Compared against the global CAPPI
    // height on display: a mismatch re-derives in the background instead
    // of showing a silently wrong altitude.
    double cappiHeightM = std::numeric_limits<double>::quiet_NaN();
};

struct RadarSession {
    QString radarId;
    QString pendingVolumeId;
    DecodedChunk pendingVolume;
    int pendingChunkCount = 0;
    int pendingFailedChunks = 0;
    int pendingDecodeTasks = 0;
    int pendingDecodedChunks = 0;
    std::size_t pendingPartialRays = 0;  // rays at the last progressive publish
    bool pendingVolumeNetworkReady = false;
    QSet<QString> derivingVolumeIds;
    QString pendingReadyVolumeId;
    int pendingReadyChunkCount = 0;
    int pendingReadyFailedChunks = 0;
    QDateTime pendingReadyReceivedAt;
    std::vector<RadarFrame> history;
    int frameIndex = -1;
    QSet<QString> bookmarks;  // volumeIds starred on the filmstrip
    bool followLatest = true;
    bool requestActive = false;
    bool historyRequested = false;
    bool historyLoadStarted = false;
    bool playWhenHistoryReady = false;
};

// Spilled tail length beyond the RAM window: 8 volumes ≈ 130-250 MB on disk
// worst case, bounded and LRU-dropped from the front.
inline constexpr int kSpilledKeepVolumes = 8;

QString spillFilePath(const QString& radarId, const QString& volumeId);

// Scrub hit on a spilled frame faults back in synchronously and re-enforces
// the budget. Null when missing/unreadable (path cleared so we stop retrying).
std::shared_ptr<DecodedChunk> sessionFrameChunk(RadarSession& session, std::size_t index,
                                               int maximumHistoryFrames);
// Keeps the newest maximumHistoryFrames volumes resident, spills older ones
// to disk (up to kSpilledKeepVolumes), drops the rest entirely.
void enforceSessionSpillBudget(RadarSession& session, int maximumHistoryFrames, int excludeIndex = -1);

}  // namespace cappedweather
