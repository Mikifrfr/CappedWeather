#pragma once

#include "core/level2_decoder.hpp"

#include <QObject>
#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QThreadPool>

#include <memory>
#include <QVector>
#include <QUrl>

#include <optional>

namespace cappedweather {

class NexradSource final : public QObject {
    Q_OBJECT

public:
    explicit NexradSource(QObject* parent = nullptr);

public slots:
    void requestCatalog();
    void requestLatest(const QString& radarId);
    void requestRecent(const QString& radarId, int volumeCount = 6);
    void clearDownloaded(const QString& radarId);
    void cancel(const QString& radarId);

signals:
    void volumeStarted(QString radarId, QString volumeId, int chunkCount);
    void volumeProgress(QString radarId, QString volumeId, int completed, int total,
                        int decoded, int failed);
    void chunkDecoded(QString radarId, QString volumeId, QString key,
                      std::shared_ptr<cappedweather::DecodedChunk> decoded,
                      QDateTime receivedAt);
    void volumeReady(QString radarId, QString volumeId, int chunkCount, int failedChunks,
                     QDateTime receivedAt);
    void catalogReceived(QByteArray data);
    void statusChanged(QString radarId, QString status);
    void requestFailed(QString radarId, QString message);

private slots:
    void catalogFinished(QNetworkReply* reply);
    void rootListingFinished(QNetworkReply* reply);
    void volumeListingFinished(QNetworkReply* reply);
    void objectFinished(QNetworkReply* reply);

private:
    struct PendingObject {
        QString radarId;
        QString key;
        QString lastModified;
        quint64 size = 0;
    };

    struct ListingContext {
        QString radarId;
        QString startAfter;
        QStringList volumePrefixes;
        int requestedCount = 1;
    };

    struct VolumeListingContext {
        QString radarId;
        QString volumePrefix;
        QString startAfter;
        QStringList volumePrefixes;
        int candidateIndex = 0;
        int requestedCount = 1;
        int completedCount = 0;
        QVector<PendingObject> objects;
    };

    struct ObjectContext {
        PendingObject object;
        QString volumeId;
        int attempt = 0;
    };

    struct ActiveVolume {
        QString radarId;
        QString volumeId;
        int remaining = 0;
        int decodedChunks = 0;
        int failedChunks = 0;
        QString firstError;
        QDateTime dataTime;
        QStringList volumePrefixes;
        int candidateIndex = 0;
        int requestedCount = 1;
        int completedCount = 0;
    };

    void startRootListing(const QString& radarId, const QString& startAfter, QStringList volumePrefixes,
                          int requestedCount);
    void startVolumeListing(const QString& radarId, const QStringList& volumePrefixes, int candidateIndex,
                            int requestedCount, int completedCount);
    void requestVolumePage(VolumeListingContext context);
    void beginVolume(const QString& radarId, const QString& volumeId, QVector<PendingObject> objects);
    void requestObject(ObjectContext context);
    void completeObject(ObjectContext context, QDateTime receivedAt,
                        std::optional<DecodedChunk> decoded, QString failure);
    void pumpObjectQueue();
    void finishRadar(const QString& radarId);
    static QString utcPrefix(const QString& radarId);
    static QUrl objectUrl(const QString& key);

    QNetworkAccessManager network_;
    QHash<QNetworkReply*, bool> catalogRequests_;
    QHash<QNetworkReply*, ListingContext> rootListingRequests_;
    QHash<QNetworkReply*, VolumeListingContext> volumeListingRequests_;
    QHash<QNetworkReply*, ObjectContext> objectRequests_;
    QQueue<ObjectContext> queuedObjects_;
    QHash<QString, ActiveVolume> activeVolumes_;
    QSet<QString> activeRadars_;
    QSet<QString> cancelledRadars_;
    QHash<QString, QSet<QString>> downloadedVolumeIds_;
    QThreadPool processingPool_;
    int activeObjectRequests_ = 0;
    int pendingProcessingTasks_ = 0;
};

} // namespace cappedweather
