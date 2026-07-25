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
int nwsOfficialAlertStyleCount();

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
    };

    struct RequestContext {
        RequestKind kind = RequestKind::Alerts;
        int reportSet = -1;
        StormReportKind reportKind = StormReportKind::Wind;
        QDate convectiveDate;
        QString zoneUrl;
    };

    void request(const QUrl& url, RequestContext context, const QByteArray& accept = {});
    void requestFinished(QNetworkReply* reply);
    void publishIfComplete();
    void schedulePublish();
    void publishOverlaySnapshot();

    QNetworkAccessManager network_;
    QHash<QNetworkReply*, RequestContext> requests_;
    QVector<WeatherWarning> warnings_;
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
