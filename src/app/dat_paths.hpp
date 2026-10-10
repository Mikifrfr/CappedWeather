#pragma once

// NWS Damage Assessment Toolkit tornado paths: post-event survey
// centerlines from the open DAT FeatureServer (verified live 2026-10-10,
// no key). Layer 1 (Damage Lines) queried as GeoJSON in lon/lat with a
// 90-day recency bound (the service caps at 2000 records and flags
// exceededTransferLimit, so unbounded queries silently truncate).
// Properties carry efscale ("EF0".."EF5"), stormdate (epoch milliseconds),
// and start/end coordinates. EF colors are an app severity ramp
// (green-yellow-orange-red-purple-white), documented as such: DAT has no
// single published hex table to pin. Anything malformed fails closed.
//
// Pure data + parsing here (testable without a window); fetching lives in
// DatPathSource below.

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVector>

namespace cappedweather {

struct DatPath {
    int objectId = 0;
    QString efScale;  // "EF0".."EF5", may be empty
    QDateTime stormDate;
    double startLatitude = 0.0;
    double startLongitude = 0.0;
    double endLatitude = 0.0;
    double endLongitude = 0.0;
    QVector<QPointF> line;  // lon/lat survey centerline
};

// DAT lines GeoJSON: drawable LineStrings only, never fatal.
QVector<DatPath> parseDatPaths(const QByteArray& data, QString* error);
// App EF severity ramp for path colors (not official DAT symbology).
void datPathColors(const QString& efScale, QString& stroke, QString& fill);

class DatPathSource : public QObject {
    Q_OBJECT
public:
    explicit DatPathSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);

signals:
    void pathsReady(QVector<DatPath> paths);
    void statusChanged(const QString& status);

private:
    bool enabled_ = true;
    bool pending_ = false;
    QVector<DatPath> paths_;
    QNetworkAccessManager network_;
};

}  // namespace cappedweather
