#pragma once

#include <QDateTime>
#include <QDate>
#include <QColor>
#include <QHash>
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
    // Parses a Meteoalarm legacy Atom feed (feeds.meteoalarm.org,
    // per-country "meteoalarm-legacy-atom-<slug>") into WeatherWarnings.
    // CAP severity vocabulary (Minor/Moderate/Severe/Extreme) already matches
    // the NWS scale consumed by AlertCenter, so no new alert machinery is
    // needed. Entries carry EMMA/NUTS geocodes but no polygons (Meteoalarm
    // geometry lives in a separate geocode database); returned warnings have
    // empty polygons and rely on AlertCenter's zone-only path. countrySlug is
    // the feed slug (e.g. "france"), recorded in senderName for provenance.
    static QVector<WeatherWarning> parseMeteoalarmAtom(const QByteArray& data,
                                                       const QString& countrySlug,
                                                       QString* error = nullptr);

public slots:
    void refresh();

signals:
    void overlaysReady(QVector<WeatherWarning> warnings, QVector<StormReport> reports,
                       QDateTime receivedAt);
    void statusChanged(QString status);

private:
    enum class RequestKind {
        Alerts,
        Reports,
        Zone,
        Meteoalarm,
    };

    struct RequestContext {
        RequestKind kind = RequestKind::Alerts;
        int reportSet = -1;
        StormReportKind reportKind = StormReportKind::Wind;
        QDate convectiveDate;
        QString zoneUrl;
        QString meteoalarmSlug;
    };

    void request(const QUrl& url, RequestContext context, const QByteArray& accept = {});
    void requestFinished(QNetworkReply* reply);
    void publishIfComplete();
    void schedulePublish();
    void publishOverlaySnapshot();

    QNetworkAccessManager network_;
    QHash<QNetworkReply*, RequestContext> requests_;
    QVector<WeatherWarning> warnings_;
    // Per-feed Meteoalarm warning sets (keyed by feed slug). Each successful
    // response replaces its own set wholesale so a failed feed keeps its
    // last-known-good warnings instead of blanking Europe off the alert list.
    QHash<QString, QVector<WeatherWarning>> meteoalarmSets_;
    QVector<StormReport> reports_;
    std::array<QVector<StormReport>, 6> reportSets_;
    QHash<QString, QVector<int>> zoneWarningIndexes_;
    QHash<QString, QVector<WarningPolygon>> zoneCache_;
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

} // namespace cappedweather
