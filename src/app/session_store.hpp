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
    // Which product kinds this volume carries, cached when the volume is
    // installed. Product availability is asked once per displayed frame, and
    // asking frameChunk() for it faults a spilled volume back from disk on the
    // UI thread (then spills another to make room) — disk thrash during
    // playback. The mask is derived from decoded rays only, so it stays valid
    // while the frame is spilled. productsKnown separates "carries nothing"
    // (mask 0, known) from "never computed" (unknown: callers keep everything
    // enabled rather than guess).
    std::uint16_t availableProducts = 0;
    bool productsKnown = false;
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

// Which product kinds at least one ray actually carries, as a bitmask over
// ProductKind (bit i == ProductKind i). Pure and decoder-only: it needs no chunk
// residency, so a frame that has spilled to disk can still answer "what do I
// carry" without a synchronous volume read. Zero means no product is present,
// which is a real answer for an empty volume — use RadarFrame::productsKnown to
// tell that apart from a mask that was never computed.
std::uint16_t availableProductMask(const DecodedChunk& chunk);

// Scrub hit on a spilled frame faults back in synchronously and re-enforces
// the budget. Null when missing/unreadable (path cleared so we stop retrying).
std::shared_ptr<DecodedChunk> sessionFrameChunk(RadarSession& session, std::size_t index,
                                               int maximumHistoryFrames);
// Keeps the newest maximumHistoryFrames volumes resident, spills older ones
// to disk (up to kSpilledKeepVolumes), drops the rest entirely.
void enforceSessionSpillBudget(RadarSession& session, int maximumHistoryFrames, int excludeIndex = -1);
// Background spill-prefetch install: drops an off-thread loadSpilledVolume()
// result into the frame still carrying volumeId. Matches by volumeId (not
// index) because tail-drop erases the front and shifts every index while the
// disk read is in flight. No-ops when the session no longer holds the
// volume, the frame is already resident, or chunk is null. Re-enforces the
// budget excluding the installed frame. Returns true when installed.
bool installPrefetchedChunk(RadarSession& session, const QString& volumeId,
                            std::shared_ptr<DecodedChunk> chunk, int maximumHistoryFrames);

}  // namespace cappedweather
