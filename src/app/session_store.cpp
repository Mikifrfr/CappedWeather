#include "app/session_store.hpp"

#include "core/volume_spill.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>

namespace cappedweather {

QString spillFilePath(const QString& radarId, const QString& volumeId) {
    const auto sanitize = [](const QString& raw, const QString& fallback) {
        QString safe;
        safe.reserve(raw.size());
        for (const auto ch : raw) {
            safe.push_back(ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_') ||
                                   ch == QLatin1Char('.')
                               ? ch
                               : QLatin1Char('_'));
        }
        if (safe.isEmpty()) safe = fallback;
        return safe;
    };
    const QString safeRadar = sanitize(radarId, QStringLiteral("radar"));
    const QString safe = sanitize(volumeId, QStringLiteral("volume"));
    const auto base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    // Empty CacheLocation (portable/offline profiles): return a path that
    // fails visibly at spill time instead of writing toward filesystem root.
    // Callers treat spill failure as keep-resident, so RAM pressure beats
    // silent loss.
    if (base.isEmpty()) return {};
    return base + QStringLiteral("/volumes/") + safeRadar + QLatin1Char('/') + safe +
           QStringLiteral(".cwdv");
}

std::uint16_t availableProductMask(const DecodedChunk& chunk) {
    // Union over rays: a moment counts as present when any ray carries gates for
    // it, which is the same predicate the availability loop used to run inline
    // (DecodedRay::has is exactly "fields[kind] is non-empty"). Keeping it in one
    // place means the cached mask and the old per-frame scan cannot drift.
    std::uint16_t mask = 0;
    for (const auto& ray : chunk.rays) {
        for (std::size_t index = 0; index < ray.fields.size(); ++index) {
            if (!ray.fields[index].empty()) mask |= static_cast<std::uint16_t>(1U << index);
        }
    }
    return mask;
}

std::shared_ptr<DecodedChunk> sessionFrameChunk(RadarSession& session, std::size_t index,
                                               int maximumHistoryFrames) {
    if (index >= session.history.size()) return nullptr;
    auto& frame = session.history[index];
    if (frame.chunk) return frame.chunk;
    if (frame.spilledPath.isEmpty()) return nullptr;
    auto loaded = loadSpilledVolume(frame.spilledPath.toStdString());
    if (!loaded) {
        QFile::remove(frame.spilledPath);
        frame.spilledPath.clear();
        return nullptr;
    }
    frame.chunk = std::move(loaded);
    enforceSessionSpillBudget(session, maximumHistoryFrames, static_cast<int>(index));
    return frame.chunk;
}

void enforceSessionSpillBudget(RadarSession& session, int maximumHistoryFrames, int excludeIndex) {
    const auto ramWindow = static_cast<std::size_t>(std::max(2, maximumHistoryFrames));
    for (;;) {
        std::size_t resident = 0;
        int oldestResident = -1;
        for (std::size_t i = 0; i < session.history.size(); ++i) {
            if (session.history[i].chunk) {
                ++resident;
                if (oldestResident < 0 && static_cast<int>(i) != excludeIndex)
                    oldestResident = static_cast<int>(i);
            }
        }
        if (resident <= ramWindow || oldestResident < 0) break;
        auto& frame = session.history[static_cast<std::size_t>(oldestResident)];
        const auto path = spillFilePath(session.radarId, frame.volumeId);
        if (path.isEmpty()) break;  // no cache location: keep resident
        QDir().mkpath(QFileInfo(path).absolutePath());
        // A failed spill (disk full) keeps the frame resident and stops:
        // growing RAM beats losing the volume.
        if (!spillVolume(*frame.chunk, path.toStdString())) break;
        frame.chunk.reset();
        frame.spilledPath = path;
    }
    while (session.history.size() > ramWindow + static_cast<std::size_t>(kSpilledKeepVolumes)) {
        auto& front = session.history.front();
        if (!front.spilledPath.isEmpty()) QFile::remove(front.spilledPath);
        session.history.erase(session.history.begin());
        // Erasing the front shifts every index down by one. Without this
        // the timeline keeps pointing at the next volume over.
        if (session.frameIndex > 0) {
            --session.frameIndex;
        } else if (session.frameIndex == 0) {
            session.frameIndex = session.history.empty() ? -1 : 0;
        }
        if (session.frameIndex >= static_cast<int>(session.history.size()))
            session.frameIndex = static_cast<int>(session.history.size()) - 1;
    }
}

bool installPrefetchedChunk(RadarSession& session, const QString& volumeId,
                            std::shared_ptr<DecodedChunk> chunk, int maximumHistoryFrames) {
    if (!chunk || volumeId.isEmpty()) return false;
    int found = -1;
    for (int i = 0; i < static_cast<int>(session.history.size()); ++i) {
        if (session.history.at(static_cast<std::size_t>(i)).volumeId == volumeId) {
            found = i;
            break;
        }
    }
    if (found < 0) return false;
    auto& frame = session.history.at(static_cast<std::size_t>(found));
    if (frame.chunk) return false;  // already resident: someone faulted it in
    if (frame.spilledPath.isEmpty()) return false;  // dropped: do not resurrect
    frame.chunk = std::move(chunk);
    enforceSessionSpillBudget(session, maximumHistoryFrames, found);
    return true;
}

std::vector<RainfallFrameSnapshot> rainfallFrameSnapshot(const RadarSession& session,
                                                         const int maximumVolumes) {
    std::vector<RainfallFrameSnapshot> frames;
    const auto total = static_cast<int>(session.history.size());
    if (total <= 0) return frames;
    const auto limit = std::max(1, maximumVolumes);
    const auto first = std::max(0, total - limit);
    frames.reserve(static_cast<std::size_t>(total - first));
    for (int index = first; index < total; ++index) {
        const auto& frame = session.history.at(static_cast<std::size_t>(index));
        RainfallFrameSnapshot snapshot;
        snapshot.volumeId = frame.volumeId;
        snapshot.dataTime = frame.dataTime;
        snapshot.chunk = frame.chunk;
        snapshot.spilledPath = frame.spilledPath;
        frames.push_back(std::move(snapshot));
    }
    return frames;
}

RainfallHistoryResult accumulateRainfallHistory(const std::vector<RainfallFrameSnapshot>& frames,
                                                const double radarElevationMeters,
                                                const RainfallConfig& config) {
    RainfallHistoryResult result;
    result.frameCount = static_cast<int>(frames.size());
    if (frames.empty()) return result;

    // Faulted-in spills have to outlive the accumulation call: RainfallSample
    // borrows the chunk, so this vector is the owner for the duration.
    std::vector<std::shared_ptr<DecodedChunk>> faulted;
    std::vector<RainfallSample> samples;
    faulted.reserve(frames.size());
    samples.reserve(frames.size());
    for (const auto& frame : frames) {
        // No frame time means no honest interval: the spill format cannot
        // supply one and mtime is not a scan clock. Skip, never guess.
        if (!frame.dataTime.isValid()) continue;
        std::shared_ptr<DecodedChunk> chunk = frame.chunk;
        if (!chunk) {
            if (frame.spilledPath.isEmpty()) continue;
            chunk = loadSpilledVolume(frame.spilledPath.toStdString());
            if (!chunk) {
                ++result.unreadableSpillCount;
                continue;
            }
            faulted.push_back(chunk);
        }
        RainfallSample sample;
        sample.chunk = chunk.get();
        sample.secondsSinceEpoch = static_cast<double>(frame.dataTime.toSecsSinceEpoch());
        samples.push_back(sample);
        if (!result.oldestVolumeTime.isValid() || frame.dataTime < result.oldestVolumeTime)
            result.oldestVolumeTime = frame.dataTime;
        if (!result.newestVolumeTime.isValid() || frame.dataTime > result.newestVolumeTime)
            result.newestVolumeTime = frame.dataTime;
        result.radarId = chunk->radarId.empty() ? result.radarId
                                                : QString::fromStdString(chunk->radarId);
    }
    result.grid = accumulateRainfall(samples, radarElevationMeters,
                                     std::numeric_limits<double>::quiet_NaN(), config);
    return result;
}

}  // namespace cappedweather
