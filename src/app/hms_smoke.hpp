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

// One day's smoke KML: plumes with drawable rings. Empty file (clean air)
// parses to zero plumes with a clear error string.
QVector<HmsSmokePlume> parseHmsSmokeKml(const QByteArray& data, QString* error);
// Day-stamped KML URL for a UTC date (monthly subdirectories).
QUrl hmsSmokeUrl(QDate date);
QString hmsSmokeDensityLabel(HmsSmokeDensity density);

class HmsSmokeSource : public QObject {
    Q_OBJECT
public:
    explicit HmsSmokeSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);

signals:
    void smokeReady(QVector<HmsSmokePlume> plumes);
    void statusChanged(const QString& status);

private:
    void requestDay(QDate date, bool fallbackLeft);

    bool enabled_ = true;
    bool pending_ = false;
    QVector<HmsSmokePlume> plumes_;
    QNetworkAccessManager network_;
};

}  // namespace cappedweather
