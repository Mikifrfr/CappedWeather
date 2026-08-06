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
    void requestAws(const QString& radarId, const QDateTime& startUtc,
                    const QDateTime& endUtc, int maximumVolumes = 0);
    void cancel();

signals:
    void volumeStarted(QString radarId, QString volumeId, int chunkCount);
    void volumeProgress(QString radarId, QString volumeId, int completed, int total,
                        int decoded, int failed);
    void chunkDecoded(QString radarId, QString volumeId, QString key,
                      cappedweather::DecodedChunk decoded, QDateTime receivedAt);
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
