#pragma once

// METAR surface observations from the aviationweather.gov JSON API. Two
// requests per refresh: stationinfo (static table, bbox in lat,lon order)
// for the station catalog around the selected radar, then one batched metar
// ids query for the newest observation per station. Anything malformed or
// any HTTP error fails closed: the last-known-good markers stay painted and
// only the status line changes.
//
// Pure data + parsing here (testable without a window); fetching lives in
// MetarSource below.

#include <QColor>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVector>

#include <limits>

namespace cappedweather {

struct MetarStation {
    QString icaoId;
    double latitude = 0.0;
    double longitude = 0.0;
    QString site;
    int priority = 9;  // aviationweather 1 (major) .. 9 (minor)
    bool hasMetar = false;
    bool hasCoords = false;
};

struct MetarObs {
    QString icaoId;
    QString stationName;
    double latitude = 0.0;
    double longitude = 0.0;
    QDateTime obsTime;
    double tempC = std::numeric_limits<double>::quiet_NaN();
    double dewpointC = std::numeric_limits<double>::quiet_NaN();
    int windDirectionDegrees = -1;  // -1 when calm, variable, or missing
    int windSpeedKnots = -1;
    int windGustKnots = -1;
    QString flightCategory;  // VFR/MVFR/IFR/LIFR, may be empty
};

// Station catalog parse: keeps entries with an ICAO id, coordinates, and a
// METAR in siteType. Null entries are dropped, never fatal.
QVector<MetarStation> parseMetarStations(const QByteArray& data, QString* error);
// Observation parse: one row per station (newest obsTime wins when the feed
// returns two hourly reports per station). Rows without an ICAO or without
// coordinates are dropped. Missing temp/dew/wind stay NaN/-1, never zero.
QVector<MetarObs> parseMetarObs(const QByteArray& data, QString* error);

// Flight-category marker color. Unknown/empty categories map to gray so an
// unrecognized string never paints as VFR green.
QColor metarCategoryColor(const QString& flightCategory);

class MetarSource : public QObject {
    Q_OBJECT
public:
    explicit MetarSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);
    // Surface region to cover: stations within +-halfDegrees of the center.
    // Called on radar selection; the overlay timer reuses the last region.
    void setRegion(double latitude, double longitude, double halfDegrees = 3.0);

signals:
    void metarsReady(QVector<MetarObs> metars);
    void statusChanged(const QString& status);

private:
    void fetchStations();
    void fetchObs();

    bool enabled_ = true;
    bool pending_ = false;
    double regionLatitude_ = 38.0;
    double regionLongitude_ = -96.0;
    double regionHalfDegrees_ = 8.0;
    QVector<MetarStation> stations_;
    QVector<MetarObs> metars_;
    QNetworkAccessManager network_;
};

}  // namespace cappedweather
