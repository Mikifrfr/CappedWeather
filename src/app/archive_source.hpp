#pragma once

#include "app/archive_level2.hpp"

#include <QObject>
#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QQueue>
#include <QThreadPool>

#include <memory>

class QNetworkReply;

namespace cappedweather {

class ArchiveSource final : public QObject {
    Q_OBJECT

public:
    explicit ArchiveSource(QObject* parent = nullptr);
    ~ArchiveSource() override;

public slots:
    void loadFiles(const QString& radarId, const QStringList& paths);
    // Background-prefetch hook (future, no logic yet): a SessionPrefetcher
    // pick-list warmer for spilled volumes would NOT go through requestAws();
    // it faults local .cwdv spill files via loadSpilledVolume() off the UI
    // thread instead. If AWS pre-warming is ever added, add a separate
    // low-priority slot here that builds a Job with maximumVolumes capped and
    // reuses pump()/processingPool_ concurrency rather than changing this path.
    void requestAws(const QString& radarId, const QDateTime& startUtc,
                    const QDateTime& endUtc, int maximumVolumes = 0);
    void cancel();

signals:
    void volumeStarted(QString radarId, QString volumeId, int chunkCount);
    void volumeProgress(QString radarId, QString volumeId, int completed, int total,
                        int decoded, int failed);
    void chunkDecoded(QString radarId, QString volumeId, QString key,
                      std::shared_ptr<cappedweather::DecodedChunk> decoded, QDateTime receivedAt);
    void volumeReady(QString radarId, QString volumeId, int chunkCount, int failedChunks,
                     QDateTime receivedAt);
    void statusChanged(QString radarId, QString status);
    void requestFailed(QString radarId, QString message);

private:
    struct Job {
        QString radarId;
        QDateTime startUtc;
        QDateTime endUtc;
        int maximumVolumes = 0;
        int pendingListings = 0;
        int activeDownloads = 0;
        int pendingDecodes = 0;
        int completed = 0;
        int decoded = 0;
        int failed = 0;
        bool cancelled = false;
        QVector<AwsArchiveObject> listed;
        QQueue<AwsArchiveObject> queued;
    };

    struct ObjectRequest {
        std::shared_ptr<Job> job;
        AwsArchiveObject object;
    };

    void listingFinished(QNetworkReply* reply);
    void objectFinished(QNetworkReply* reply);
    // Background-prefetch hook (future, no logic yet): beginDownloads()/pump()
    // are the reuse points for any AWS prefetch — a pick-list-driven Job would
    // enqueue here at lower priority than the explicit user request, honour
    // archiveParallelDownloads(), the kMaximumArchiveBytes guard, and cancel().
    // Spill-file warming (SessionPrefetcher) intentionally bypasses this class.
    void beginDownloads(const std::shared_ptr<Job>& job);
    void pump(const std::shared_ptr<Job>& job);
    void completeObject(const std::shared_ptr<Job>& job, const AwsArchiveObject& object,
                        std::optional<ArchiveLevel2Result> decoded, QString error);
    void finishIfDone(const std::shared_ptr<Job>& job);

    QNetworkAccessManager network_;
    QHash<QNetworkReply*, std::shared_ptr<Job>> listingRequests_;
    QHash<QNetworkReply*, ObjectRequest> objectRequests_;
    QVector<std::shared_ptr<Job>> jobs_;
    QThreadPool processingPool_;
    quint64 localGeneration_ = 0;
};

} // namespace cappedweather
