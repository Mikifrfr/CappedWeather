#pragma once

#include <QDateTime>
#include <QDate>
#include <QColor>
#include <QHash>
#include <QJsonValue>
#include <QSet>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QUrl>
#include <QVector>

#include <array>

class QNetworkReply;
class QTimer;

namespace cappedweather {

struct NwsAlertStyle {
    QColor color;
    int priority = 1000;
};

NwsAlertStyle nwsAlertStyle(const QString& event);

struct WarningPolygon {
    QPolygonF exterior; // longitude, latitude
    QVector<QPolygonF> holes;
};

struct WeatherWarning {
    // Stable NWS @id from the alert feature. Used by AlertCenter to dedupe
    // updates that reissue the same alert. Empty when the API omits the field.
    QString id;
    QString event;
    QString headline;
    QString severity;
    QString senderName;
    QString description;
    QString instruction;
    QDateTime onset;
    QDateTime expires;
    QColor alertColor;
    int alertPriority = 1000;
    QVector<WarningPolygon> polygons;
    QStringList affectedZones;
};

enum class StormReportKind {
    Tornado,
    Hail,
    Wind,
};

struct StormReport {
    StormReportKind kind = StormReportKind::Wind;
    QPointF coordinate; // longitude, latitude
    QString time;
    QDateTime occurredAt;
    QString magnitude;
    QString location;
    QString state;
    QString comments;
};

class WeatherOverlaySource final : public QObject {
    Q_OBJECT

public:
    explicit WeatherOverlaySource(QObject* parent = nullptr);

    static QVector<WeatherWarning> parseAlerts(const QByteArray& data, QString* error = nullptr);
    static QVector<WarningPolygon> parseZone(const QByteArray& data, QString* error = nullptr);
    static QVector<StormReport> parseStormReports(const QByteArray& data, StormReportKind kind,
                                                   QString* error = nullptr,
                                                   QDate convectiveDate = {});
    // Meteoalarm warning geometry over the EUMETNET visualization service
    // (visservice.meteoalarm.org, same backend as meteoalarm.org). Warnings
    // carry EMMA/NUTS geocodes but no polygons; official area polygons ride
    // gzip-compressed Mapbox Vector Tiles, joined to warnings by area UUID:
    // regional hazards list warned areas (uuid, name, bbox) plus one hazard
    // entry per (area, type, level), per-area details carry the warning text,
    // and the areas layer of covering z5 tiles carries the polygons.
    // Hour-truncated UTC reference date shared by one refresh round.
    static QDateTime meteoReferenceDate(const QDateTime& now);
    // Maps a visservice awareness levelId onto the NWS severity scale
    // consumed by AlertCenter (1=Extreme/red, 2=Severe/orange, 3=Moderate/
    // yellow) with official Meteoalarm colors.
    static NwsAlertStyle meteoalarmLevelStyle(int levelId);
    static QString meteoSeverityForLevel(int levelId);
    // Tile URL + covering-tile computation for the areas layer (z5).
    static QUrl meteoTileUrl(int z, int x, int y, const QDateTime& referenceDate);
    static QVector<QPair<int, int>> meteoTilesForBounds(double minLon, double minLat,
                                                        double maxLon, double maxLat);
    // Decodes warned-area polygons from one gzip MVT tile (areas layer only).
    // wantedUuids selects areas; anything else is skipped without decoding
    // geometry. Returns uuid -> polygons. Needs zlib; without it every call
    // fails closed, see meteoTilesAvailable().
    static QHash<QString, QVector<WarningPolygon>> parseMvtAreas(const QByteArray& gzipData,
                                                                 int z, int x, int y,
                                                                 const QSet<QString>& wantedUuids,
                                                                 QString* error = nullptr);

public slots:
    // Offline last-known-good: refresh() never clears published state up
    // front. Report sets, warnings_, and the Meteoalarm per-country
    // area/hazard sets plus detail/tile caches all survive until a success
    // replaces them wholesale (see refresh()/requestFinished()), so an
    // offline round keeps the previous overlays on screen and only flips
    // statusChanged() to "partial". ETag/If-Modified-Since revalidation plus
    // the 304-cached-body path in requestFinished() serves the same goal.
    void refresh();

signals:
    // Offline last-known-good emission: overlaysReady() always carries the
    // most recent consistent snapshot (stale warnings_ + whatever report sets
    // succeeded), even when the alerts request or its parse fails — see
    // publishIfComplete(). Consumers must treat this as usable offline data,
    // not an error clear; failures are reported via statusChanged() only.
    void overlaysReady(QVector<WeatherWarning> warnings, QVector<StormReport> reports,
                       QDateTime receivedAt);
    void statusChanged(QString status);

private:
    enum class RequestKind {
        Alerts,
        Reports,
        Zone,
        MeteoRegions,
        MeteoDetails,
        MeteoTiles,
        MeteoTypes,
    };

    struct RequestContext {
        RequestKind kind = RequestKind::Alerts;
        int reportSet = -1;
        StormReportKind reportKind = StormReportKind::Wind;
        QDate convectiveDate;
        QString zoneUrl;
        // MeteoRegions: visservice region code (e.g. "FR").
        // MeteoDetails: cache key "featureId|typeId|levelId" + payload.
        // MeteoTiles: "z/x/y" key + coordinates.
        QString meteoArg;
        QString meteoFeature;
        int meteoType = 0;
        int meteoLevel = 0;
        int meteoZ = 0;
        int meteoX = 0;
        int meteoY = 0;
    };

    void request(const QUrl& url, RequestContext context, const QByteArray& accept = {});
    void requestFinished(QNetworkReply* reply);
    void publishIfComplete();
    void schedulePublish();
    void publishOverlaySnapshot();
    // Meteoalarm follow-ups: warning details for new hazard signatures and
    // vector tiles covering warned bboxes (both bounded per round).
    void requestMeteoFollowUps();
    void pruneMeteoTileCache();
    // Assembles publishable European warnings from hazard sets + cached
    // details + cached tile geometry. A warned area publishes once its
    // details arrive; polygons attach (via republish) when tiles land.
    QVector<WeatherWarning> buildMeteoWarnings() const;
    static QString localizedText(const QJsonValue& value);

    QNetworkAccessManager network_;
    QHash<QNetworkReply*, RequestContext> requests_;
    // Offline last-known-good store for NWS alerts: replaced wholesale only
    // on a successful parse (see requestFinished()); a failed round leaves
    // the previous warnings_ in place so the map never blanks while offline.
    QVector<WeatherWarning> warnings_;
    // Meteoalarm state. Regional responses replace their own per-country sets
    // wholesale (last-known-good survives per-feed failures); details and
    // tile geometry accumulate in caches keyed by warning signature and tile.
    struct MeteoArea {
        QString name;
        QString region;
        double minLon = 0.0;
        double minLat = 0.0;
        double maxLon = 0.0;
        double maxLat = 0.0;
    };
    struct MeteoHazard {
        QString featureId;
        int typeId = 0;
        int levelId = 0;
    };
    struct MeteoDetail {
        QString warningId;
        QString headline;
        QString description;
        QString instruction;
        QString senderName;
        QDateTime onset;
        QDateTime expires;
    };
    QHash<QString, QHash<QString, MeteoArea>> meteoAreaSets_;
    QHash<QString, QVector<MeteoHazard>> meteoHazardSets_;
    QHash<QString, MeteoDetail> meteoDetails_;
    QHash<QString, QDateTime> meteoDetailFetchedAt_;
    QHash<QString, QHash<QString, QVector<WarningPolygon>>> meteoTilePolygons_;
    QHash<QString, QString> meteoTypeNames_;
    bool meteoTypesRequested_ = false;
    // In-flight dedupe within one refresh round.
    QSet<QString> meteoPendingDetails_;
    QSet<QString> meteoPendingTiles_;
    QDateTime meteoReferenceDate_;
    QVector<StormReport> reports_;
    // Offline last-known-good store for SPC reports: each of the 6
    // (today/yesterday x tornado/hail/wind) sets is replaced wholesale only
    // on its own success; a failed fetch keeps that set's previous contents
    // and publishOverlaySnapshot() re-emits the merged 24 h view unchanged.
    std::array<QVector<StormReport>, 6> reportSets_;
    QHash<QString, QVector<int>> zoneWarningIndexes_;
    QHash<QString, QVector<WarningPolygon>> zoneCache_;
    // Offline/304 cache: responseBodies_ holds the last good body per URL so
    // a 304 revalidation can republish without network bytes; responseEtags_/
    // responseLastModified_ feed the conditional headers set in request().
    // Zone geometry is excluded from revalidation and cached separately above.
    QHash<QUrl, QByteArray> responseBodies_;
    QHash<QUrl, QByteArray> responseEtags_;
    QHash<QUrl, QByteArray> responseLastModified_;
    QTimer* publishTimer_ = nullptr;
    bool alertResponseReady_ = false;
    int totalRequests_ = 0;
    int pending_ = 0;
    int failures_ = 0;
    QString firstError_;
};

// True when this build linked zlib and can actually decode MVT tiles.
bool meteoTilesAvailable();

} // namespace cappedweather
