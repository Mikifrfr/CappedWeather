#pragma once

// Extracted from MainWindow: per-radar volume history plus spill-to-disk.
// MainWindow keeps playback/UI; this module owns the data window so session
// logic is unit-testable without a QMainWindow.

#include "core/derived_products.hpp"
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

// Writes the volume to destPath through a unique sibling file renamed into
// place, so a reader never opens a half-written volume and two writers cannot
// truncate each other. Spill files are write-once per radar+volume: when
// destPath already holds the volume (another process got there first) the
// write is skipped and the existing file is reused. replaceExisting is for the
// derived-superset rewrite, which must replace a base spill in place.
bool spillVolumeAtomically(const DecodedChunk& chunk, const QString& destPath,
                           bool replaceExisting = false);
// Same write-once staging for a byte copy; this is the case-pack import path,
// which stages into the shared spill location.
bool stageFileAtomically(const QString& sourcePath, const QString& destPath);

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

// ---------------------------------------------------------------------------
// Z-R rainfall accumulation input (serialized for an off-thread call)
// ---------------------------------------------------------------------------
// The engine needs one sample per volume with the volume's scan time, but a
// DecodedChunk carries no time at all (and the CWDV spill format has none
// either): the only honest clock is RadarFrame::dataTime. This snapshot keeps
// that pairing explicit and is cheap to take on the GUI thread — a resident
// frame borrows its chunk, a spilled frame carries only its path and its frame
// time, so taking it never touches the disk and never shifts the RAM budget.
struct RainfallFrameSnapshot {
    QString volumeId;
    QDateTime dataTime;                 // RadarFrame::dataTime; never file mtime
    std::shared_ptr<DecodedChunk> chunk;  // null when the frame has spilled
    QString spilledPath;                // set when chunk is null and still on disk
};

// The session's window, oldest entry first, capped to the newest
// maximumVolumes (the derived history ceiling is 32 resident + 8 spilled).
// Frames that are neither resident nor spilled are skipped: a dropped volume
// has no data left to score, and its absence is already visible in the window
// length the summary reports.
std::vector<RainfallFrameSnapshot> rainfallFrameSnapshot(const RadarSession& session,
                                                         int maximumVolumes = 40);

struct RainfallHistoryResult {
    RainfallGrid grid;
    QString radarId;            // the radar the grid was built from
    QDateTime oldestVolumeTime;  // frame time of the first credited volume
    QDateTime newestVolumeTime;  // frame time of the last volume in the window
    int frameCount = 0;         // frames handed in
    int unreadableSpillCount = 0;  // frames whose disk copy failed to load
};

// Off-GUI-thread half of the readout: faults spilled frames back in from disk
// by path, pairs every volume with its frame time, and integrates the Z-R
// estimate over the newest volume's lowest tilt. Unreadable spills are counted
// and skipped, never guessed at. Touches no session state, so it is safe to run
// on a worker while playback keeps scrubbing.
RainfallHistoryResult accumulateRainfallHistory(const std::vector<RainfallFrameSnapshot>& frames,
                                                double radarElevationMeters,
                                                const RainfallConfig& config = {});

}  // namespace cappedweather
