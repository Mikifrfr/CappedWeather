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
// The bucket is heterogeneous across providers:
// - PVOL sites (e.g. Norway): one object holds a whole multi-tilt moment,
//   so a "volume" is a single download.
// - SCAN sites (France, Germany, Finland, Sweden, ...): one object holds a
//   single elevation (single or combined moments), so a volume is assembled
//   from all elevations sharing a 5-minute scan bucket (multiple downloads
//   merged with renumbered elevations).
// - Moment naming varies: "@DBZH.h5" (separate) vs "@DBZH_TH_VRADH.h5" and
//   "@CCORH_DBZH_..._ZDR.h5" (combined single objects).
// The signal surface mirrors NexradSource so MainWindow slots (timeline,
// tilts, derived products, export) work unchanged.
class OperaSource final : public QObject {
    Q_OBJECT

public:
    explicit OperaSource(QObject* parent = nullptr);

    // Pure helpers, unit-testable without network.
    static QString pvolPrefix(const QString& countryCode, const QString& odimCode,
                              const QDate& date);
    static QString scanPrefix(const QString& countryCode, const QString& odimCode,
                              const QDate& date);
    // Parses "<odim>@YYYYMMDDTHHMM@..." object keys. Invalid when absent.
    static QDateTime timestampFromKey(const QString& key);
    // True for objects carrying a DBZH moment: "@DBZH.h5" as well as combined
    // moment lists such as "@DBZH_TH_VRADH.h5" or "@CCORH_DBZH_..._ZDR.h5".
    // Still requires the "<odim>@" filename prefix so foreign sites are dropped.
    static bool isDbzhKey(const QString& key, const QString& odimCode);
    static bool isThKey(const QString& key, const QString& odimCode);
    // Groups newest-first SCAN candidates into 5-minute scan buckets (newest
    // bucket first). Each inner vector holds the files of one volume.
    static QVector<QVector<QPair<QString, QDateTime>>> groupScanCandidates(
        const QVector<QPair<QString, QDateTime>>& newestFirst, int requestedCount,
        int maxPerGroup = 20);
    // Drops the newest bucket when it is still filling (fewer files than the
    // previous bucket and minutes old: providers upload elevations over
    // several minutes) and trims to requestedCount. A filling bucket is never
    // marked downloaded, so it is picked up complete on the next refresh.
    static QVector<QVector<QPair<QString, QDateTime>>> trimScanGroups(
        QVector<QVector<QPair<QString, QDateTime>>> groups, int requestedCount,
        const QDateTime& now);
    // Merges single-elevation SCAN parts into one volume, renumbering
    // elevations sequentially sorted by angle (each part decodes standalone
    // with elevationNumber starting at 1, which would otherwise collapse).
    static DecodedChunk mergeScanParts(std::vector<DecodedChunk> parts,
                                       const std::string& radarId);

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
    struct VolumePlan {
        QString volumeId;
        QDateTime dataTime;
        QVector<QPair<QString, QDateTime>> files;
    };
    struct ListingContext {
        QString radarId;
        QString odimCode;
        QString countryCode;
        QVector<QString> prefixes;
        int prefixIndex = 0;
        int requestedCount = 1;
        QString startAfter;
        // DBZH candidates across listing pages (key, timestamp).
        QVector<QPair<QString, QDateTime>> candidates;
        // TH-only fallback when no DBZH object exists for the site.
        QVector<QPair<QString, QDateTime>> thCandidates;
        // Seen-key sets for O(1) dedupe: SCAN sites list thousands of objects
        // across dozens of pages, and a linear rescan per object is O(n^2).
        QSet<QString> seenKeys;
        QSet<QString> seenThKeys;
    };
    struct DownloadContext {
        QString radarId;
        QString volumeId;
        QString key;
        QDateTime dataTime;
        int attempt = 0;
        bool scanPart = false;
    };
    struct ScanGroupState {
        QDateTime dataTime;
        int expected = 0;
        int finished = 0;
        int failed = 0;
        std::vector<DecodedChunk> parts;
        QDateTime receivedAt;
    };

    void startListing(ListingContext context);
    void finishListing(QNetworkReply* reply);
    void beginDownloads(const QString& radarId, QVector<QPair<QString, QDateTime>> candidates,
                        int requestedCount);
    void beginScanVolumes(const QString& radarId, QVector<VolumePlan> plans);
    void startNextScanVolume(const QString& radarId);
    void requestObject(DownloadContext context);
    void finishObject(QNetworkReply* reply);
    void decodeObject(DownloadContext context, QByteArray payload);
    void completeVolume(DownloadContext context, const QDateTime& receivedAt,
                        std::optional<DecodedChunk> decoded, const QString& failure);
    void completeScanPart(DownloadContext context, const QDateTime& receivedAt,
                          std::optional<DecodedChunk> decoded);
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
    QHash<QString, QVector<VolumePlan>> pendingScanVolumes_;
    QHash<QString, ScanGroupState> activeScanGroups_;
    QSet<QString> inFlightRadars_;
    QSet<QString> cancelledRadars_;
    QThreadPool processingPool_;
};

} // namespace cappedweather
