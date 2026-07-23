#pragma once

#include "core/flash_rate_grid.hpp"
#include "core/lightning_overlay.hpp"

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVector>

#include <cstdint>
#include <vector>

class QNetworkReply;

namespace cappedweather {

struct GlmLiveConfig {
    FlashRateGridConfig grid = {};
    int refreshSeconds = 20;
    int initialBackfillMinutes = 18;
    int maximumConcurrentDownloads = 4;
    int cessationRetentionMinutes = 10;
    double satelliteSplitLongitudeDeg = -105.0;
};

// Anonymous live GOES-18/19 GLM L2 LCFA client. Each refresh is a bounded
// listing/download/parse batch. Files are ingested in scan-time order and the
// two satellites are split at a configurable meridian to avoid double-counting
// their CONUS overlap.
class GlmSource final : public QObject {
    Q_OBJECT

public:
    explicit GlmSource(QObject* parent = nullptr);

    [[nodiscard]] GlmLiveConfig config() const { return config_; }

public slots:
    void start();
    void stop();
    void refresh();
    void configure(GlmLiveConfig config);

signals:
    void cellsUpdated(QVector<cappedweather::LightningCellSnapshot> cells,
                      QDateTime latestDataTime, int filesProcessed, int flashesProcessed);
    void statusChanged(QString status);
    void requestFailed(QString message);

private:
    enum class RequestKind { Listing, Object };

    struct RequestContext {
        RequestKind kind = RequestKind::Listing;
        QString satellite;
        QString key;
        std::int64_t scanStartEpochSec = 0;
    };

    struct PendingObject {
        QString satellite;
        QString key;
        std::int64_t scanStartEpochSec = 0;
    };

    struct ParsedFile {
        QString satellite;
        QString key;
        std::int64_t scanStartEpochSec = 0;
        std::vector<LightningFlash> flashes;
    };

    void requestListing(const QString& satellite, const QDateTime& hour);
    void requestObject(PendingObject object);
    void requestFinished(QNetworkReply* reply);
    void listingFinished(QNetworkReply* reply, const RequestContext& context);
    void objectFinished(QNetworkReply* reply, RequestContext context);
    void beginDownloads();
    void pumpDownloads();
    void finishParse(RequestContext context, std::vector<LightningFlash> flashes,
                     QString error);
    void finishBatch();
    void ingestBatch();
    void publishCells(std::int64_t nowEpochSec);
    void rememberSeenKey(const QString& key);
    static QString prefixForHour(const QDateTime& hour);
    static QUrl listingUrl(const QString& satellite, const QString& prefix);
    static QUrl objectUrl(const QString& satellite, const QString& key);

    QNetworkAccessManager network_;
    QHash<QNetworkReply*, RequestContext> requests_;
    QTimer timer_;
    QThreadPool processingPool_;
    GlmLiveConfig config_;
    FlashRateGrid grid_;
    QQueue<PendingObject> downloadQueue_;
    QVector<PendingObject> candidates_;
    std::vector<ParsedFile> parsedFiles_;
    QSet<QString> seenKeys_;
    QStringList seenKeyOrder_;
    QHash<QString, std::int64_t> latestProcessedBySatellite_;
    QHash<std::int64_t, LightningCellSnapshot> cells_;
    int pendingListings_ = 0;
    int activeDownloads_ = 0;
    int pendingParses_ = 0;
    int batchFailures_ = 0;
    int batchFilesProcessed_ = 0;
    int batchFlashesProcessed_ = 0;
    bool running_ = false;
    bool batchActive_ = false;
    bool refreshPending_ = false;
    bool initialBackfillComplete_ = false;
};

} // namespace cappedweather

Q_DECLARE_METATYPE(cappedweather::LightningCellSnapshot)
Q_DECLARE_METATYPE(QVector<cappedweather::LightningCellSnapshot>)
