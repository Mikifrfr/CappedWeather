#pragma once

// WPC excessive-rainfall outlooks Day 1-3: same polygon machinery as the
// SPC outlooks (official fills, background paint, click cards), different
// office and schema. Geometry arrives as Esri REST GeoJSON (outSR=4326
// lon/lat, Polygon/MultiPolygon with occasional holes: exterior ring only,
// like the SPC path). Properties carry `outlook` risk text, `dn` severity
// (1 Marginal .. 4 High), and `start_time`/`end_time`/`issue_time` UTC
// stamps ("yyyy-MM-dd HH:mm:ss"). Fill/outline colors are the official
// WPC ERO renderer values sampled live from the MapServer drawingInfo
// (dn1 #38A800/#00734C, dn2 #FFFE00/#E69800, dn3 #F50000/#8A0000,
// dn4 #FF69C5/#FF00FF), pinned in code so a renderer restyle never
// silently recolors the map. Anything malformed fails closed to empty.
//
// Reuses SpcOutlookFeature with fireKind "ERO" (day 1-3) so canvas paint,
// click, and badges ride the proven outlook path with their own vector.

#include "app/spc_convective.hpp"

namespace cappedweather {

// WPC ERO GeoJSON for one day (1-3): nullopt entries dropped, never fatal.
QVector<SpcOutlookFeature> parseWpcEroGeoJson(const QByteArray& data, int day, QString* error);

class WpcEroSource : public QObject {
    Q_OBJECT
public:
    explicit WpcEroSource(QObject* parent = nullptr);

    void refresh();
    void setEnabled(bool enabled);

signals:
    void outlooksReady(QVector<SpcOutlookFeature> outlooks);
    void statusChanged(const QString& status);

private:
    bool enabled_ = true;
    bool pending_ = false;
    QVector<SpcOutlookFeature> outlooks_;
    QString outlooksError_;
    QNetworkAccessManager network_;
};

}  // namespace cappedweather
