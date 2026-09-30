#include "app/session_store.hpp"

#include "core/volume_spill.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>

namespace cappedweather {

QString spillFilePath(const QString& radarId, const QString& volumeId) {
    QString safe;
    safe.reserve(volumeId.size());
    for (const auto ch : volumeId) {
        safe.push_back(ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_') ||
                               ch == QLatin1Char('.')
                           ? ch
                           : QLatin1Char('_'));
    }
    if (safe.isEmpty()) safe = QStringLiteral("volume");
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
           QStringLiteral("/volumes/") + radarId + QLatin1Char('/') + safe +
           QStringLiteral(".cwdv");
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
    }
}

}  // namespace cappedweather
