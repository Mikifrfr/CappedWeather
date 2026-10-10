#pragma once

// NHC tropical cyclone context: active-storm cone + forecast-track overlays
// from the open NHC GIS feed (no key, verified live 2026-10-10 during
// Isaias/Rachel/Simon). Discovery through the active-storms KMZ
// (gis/kml/nhc.kmz -> per-storm folders with cone/track/watch NetworkLinks),
// geometry from the per-product KMZ files (inline KML Polygon rings and
// LineString track lines, lon,lat,alt triples).
//
// KMZ files unzip through the same CAPPEDWEATHER_HAS_ZLIB path as
// case-packs (fail closed with a status line when zlib is absent: the
// layer stays quiet, never half-drawn). Watches/warnings ride the NWS
// alerts feed already, so only cone + track are fetched. Anything
// malformed fails closed to empty.
//
// Pure data + parsing here (testable without a window); fetching lives in
// NhcCycloneSource below.

#include <QByteArray>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVector>

namespace cappedweather {

struct NhcStormLink {
    QString id;  // "EP202026" from the product URL
    QString name;  // "Hurricane Simon"
    QString coneUrl;
    QString trackUrl;
};

struct NhcStorm {
    QString id;
    QString name;
    QVector<QPointF> cone;  // lon/lat ring, closed implicitly
    QVector<QPointF> track;  // lon/lat forecast positions
};

// First .kml entry out of KMZ bytes. Empty + error when zlib is absent,
// when the archive is corrupt, or when caps trip (zip-bomb guard).
QByteArray extractKmlFromKmz(const QByteArray& kmz, QString* error);
// Active-storms KML (inner file of nhc.kmz): storm folders with cone/track
// NetworkLinks. Folders without both links are skipped (quiet state).
QVector<NhcStormLink> parseNhcActiveKml(const QByteArray& data, QString* error);
// First outer-ring Polygon of a cone file, lon/lat points.
QVector<QPointF> parseNhcConeRing(const QByteArray& data, QString* error);
// First LineString of a track file, lon/lat points.
QVector<QPointF> parseNhcTrackLine(const QByteArray& data, QString* error);

class NhcCycloneSource : public QObject {
    Q_OBJECT
public:
    explicit NhcCycloneSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);

signals:
    void cyclonesReady(QVector<NhcStorm> storms);
    void statusChanged(const QString& status);

private:
    void requestProducts(QVector<NhcStormLink> links);

    bool enabled_ = true;
    bool pending_ = false;
    QVector<NhcStorm> storms_;
    QNetworkAccessManager network_;
};

}  // namespace cappedweather
