#pragma once

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QVector>

class QNetworkReply;

namespace cappedweather {

struct WeatherWarning {
    QString event;
    QString headline;
    QString severity;
    QDateTime expires;
    QVector<QPolygonF> polygons; // longitude, latitude
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
    static QVector<StormReport> parseStormReports(const QByteArray& data, StormReportKind kind,
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
        TornadoReports,
        HailReports,
        WindReports,
    };

    void request(const QUrl& url, RequestKind kind, const QByteArray& accept = {});
    void requestFinished(QNetworkReply* reply);
    void publishIfComplete();

    QNetworkAccessManager network_;
    QHash<QNetworkReply*, RequestKind> requests_;
    QVector<WeatherWarning> warnings_;
    QVector<StormReport> reports_;
    int pending_ = 0;
    int failures_ = 0;
    QString firstError_;
};

} // namespace cappedweather
