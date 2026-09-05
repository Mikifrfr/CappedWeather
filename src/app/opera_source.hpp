#pragma once

#include "core/level2_decoder.hpp"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <optional>

namespace cappedweather {

// Live EUMETNET OPERA single-site volumes over anonymous CloudFerro S3.
// Unlike NEXRAD (dozens of chunks per volume), one OPERA object already
// holds a whole multi-tilt moment, so a "volume" here is a single download.
// The signal surface mirrors NexradSource so MainWindow slots (timeline,
// tilts, derived products, export) work unchanged.
class OperaSource final : public QObject {
    Q_OBJECT

public:
    explicit OperaSource(QObject* parent = nullptr);

    // Pure helpers, unit-testable without network.
    static QString pvolPrefix(const QString& countryCode, const QString& odimCode,
                              const QDate& date);
    // Parses "<odim>@YYYYMMDDTHHMM@..." object keys. Invalid when absent.
    static QDateTime timestampFromKey(const QString& key);
    static bool isDbzhKey(const QString& key, const QString& odimCode);

public slots:
    void requestLatest(const QString& radarId);
    void requestRecent(const QString& radarId, int volumeCount = 6);
    void clearDownloaded(const QString& radarId);
    void cancel(const QString& radarId);

signals:
    void volumeStarted(QString radarId, QString volumeId, int chunkCount);
    void volumeProgress(QString radarId, QString volumeId, int completed, int total,
                        int decoded, int failed);
    void chunkDecoded(QString radarId, QString volumeId, QString key, cappedweather::DecodedChunk decoded,
                      QDateTime receivedAt);
    void volumeReady(QString radarId, QString volumeId, int chunkCount, int failedChunks,
                     QDateTime receivedAt);
    void statusChanged(QString radarId, QString status);
    void requestFailed(QString radarId, QString message);

private:
    struct ListingContext {
        QString radarId;
        QString odimCode;
        QString countryCode;
        QVector<QDate> dates;
        int dateIndex = 0;
        int requestedCount = 1;
        QString startAfter;
        // Newest-first DBZH candidates across listing pages.
        QVector<QPair<QString, QDateTime>> candidates;
    };
    struct DownloadContext {
        QString radarId;
        QString volumeId;
        QString key;
        QDateTime dataTime;
        int attempt = 0;
    };

    void startListing(ListingContext context);
    void finishListing(QNetworkReply* reply);
    void beginDownloads(const QString& radarId, QVector<QPair<QString, QDateTime>> candidates,
                        int requestedCount);
    void requestObject(DownloadContext context);
    void finishObject(QNetworkReply* reply);
    void decodeObject(DownloadContext context, QByteArray payload);
    void completeVolume(DownloadContext context, const QDateTime& receivedAt,
                        std::optional<DecodedChunk> decoded, const QString& failure);
    void failRadar(const QString& radarId, const QString& message);
    static QUrl listingUrl(const QString& prefix, const QString& startAfter);
    static QUrl objectUrl(const QString& key);
    static int maxAttempts();
    static int retryBackoffMs(int attempt);

    QNetworkAccessManager network_;
    QHash<QNetworkReply*, ListingContext> listingRequests_;
    QHash<QNetworkReply*, DownloadContext> downloadRequests_;
    QHash<QString, QSet<QString>> downloadedVolumeIds_;
    QHash<QString, QVector<QPair<QString, QDateTime>>> pendingBacklog_;
    QSet<QString> inFlightRadars_;
    QSet<QString> cancelledRadars_;
    QThreadPool processingPool_;
};

} // namespace cappedweather
