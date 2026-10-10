#pragma once

// NOAA HMS smoke polygons: daily KML over anonymous HTTPS
// (satepsanone.nesdis.noaa.gov, verified live 2026-10-10), one Placemark
// per plume with an outer-ring Polygon and a CDATA description carrying
// "Density: Light/Medium/Heavy" plus satellite and start/end times.
// Official KML fills are pinned below (green/yellow/red at 0x44 alpha).
// Today's file lands mid-morning UTC, so refresh tries today first and
// falls back to yesterday (quiet days legitimately carry zero polygons:
// an empty file is a valid clean-air answer, not an error).
// Anything malformed fails closed to empty.
//
// Pure data + parsing here (testable without a window); fetching lives in
// HmsSmokeSource below.

#include <QDate>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QUrl>
#include <QVector>

#include <limits>

namespace cappedweather {

enum class HmsSmokeDensity {
    Light,
    Medium,
    Heavy,
};

struct HmsSmokePlume {
    HmsSmokeDensity density = HmsSmokeDensity::Light;
    QString satellite;  // "GOES-WEST", may be empty
    QString startText;  // raw "2026282 1220UTC" window from the description
    QString endText;
    QVector<QPointF> ring;  // lon/lat, closed implicitly
};

// Active fire detections: same HMS program, plain-text rows
// ("Lon, Lat, YearDay, Time, Satellite, Method, Ecosystem, FRP").
// FRP is megawatts, -999 when unmeasured (stays NaN, never zero).
struct HmsFirePoint {
    double latitudeDeg = 0.0;
    double longitudeDeg = 0.0;
    QString satellite;
    double frpMw = std::numeric_limits<double>::quiet_NaN();
    int yearDay = 0;
    int timeHhmm = 0;
};

// One day's smoke KML: plumes with drawable rings. Empty file (clean air)
// parses to zero plumes with a clear error string.
QVector<HmsSmokePlume> parseHmsSmokeKml(const QByteArray& data, QString* error);
// One day's fire text: detections with sane coordinates. Header and
// malformed rows are dropped, never fatal. Hard cap for hostile files
// (daily files run a few thousand rows; the cap never trips on real data).
QVector<HmsFirePoint> parseHmsFireText(const QByteArray& data, QString* error,
                                       int maxPoints = 20000);
// Day-stamped KML URL for a UTC date (monthly subdirectories).
QUrl hmsSmokeUrl(QDate date);
// Day-stamped fire-text URL for a UTC date (same monthly layout).
QUrl hmsFireUrl(QDate date);
QString hmsSmokeDensityLabel(HmsSmokeDensity density);

class HmsSmokeSource : public QObject {
    Q_OBJECT
public:
    explicit HmsSmokeSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);

signals:
    void smokeReady(QVector<HmsSmokePlume> plumes);
    void firesReady(QVector<HmsFirePoint> fires);
    void statusChanged(const QString& status);

private:
    void requestDay(QDate date, bool fallbackLeft);
    void requestFires(QDate date);

    bool enabled_ = true;
    bool pending_ = false;
    QVector<HmsSmokePlume> plumes_;
    QNetworkAccessManager network_;
};

}  // namespace cappedweather
