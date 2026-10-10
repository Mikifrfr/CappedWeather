#pragma once

#include <QByteArray>
#include <QColor>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace cappedweather {

enum class PlacefileElementKind {
    Marker,
    Text,
    Icon,
    Line,
    Polygon,
};

struct PlacefileElement {
    PlacefileElementKind kind = PlacefileElementKind::Marker;
    QVector<QPointF> points; // longitude, latitude
    QColor color = Qt::white;
    double thresholdNauticalMiles = 999.0;
    int lineWidth = 1;
    double rotationDegrees = 0.0;
    int iconFileNumber = 0;
    int iconNumber = 0;
    QString text;
    QString hoverText;
};

struct PlacefileDocument {
    QString title;
    QString source;
    int refreshSeconds = 0;
    QVector<PlacefileElement> elements;
    QStringList warnings;
};

// Parses the intentionally bounded GRLevelX subset used by CappedWeather:
// Place, Text, Icon, Line and Polygon plus their state directives. Unsupported
// directives are retained as warnings instead of invalidating useful content.
std::optional<PlacefileDocument> parsePlacefile(const QByteArray& bytes, QString* error = nullptr);

// ESRI shapefiles (.shp) as placefile documents: 2D polygon/polyline rings
// become Polygon/Line elements (one element per ring), unprojected
// longitude/latitude only. Z/M record flavors, projected coordinates, and
// attribute (.dbf) labels stay out: out-of-range rings warn and skip, and a
// file with nothing drawable fails closed. Same 2 MiB / 50k-element /
// 10k-point safety envelope as text placefiles.
bool looksLikeShapefile(const QByteArray& bytes);
std::optional<PlacefileDocument> parseShapefile(const QByteArray& bytes, QString* error = nullptr);
// Shapefile plus dBase attribute names aligned to shape records (index i
// names record i, empty when missing or deleted). Malformed names never
// fail the geometry: the document parses unnamed with a warning.
std::optional<PlacefileDocument> parseShapefileData(const QByteArray& bytes,
                                                    const QStringList& recordNames,
                                                    QString* error = nullptr);
// dBase III/IV attribute table: first character ('C') field per record,
// Latin-1, trimmed, empty for deleted records. Empty list on any malformed
// input (never fatal: callers fall back to unnamed geometry).
QStringList parseDbfNames(const QByteArray& bytes);

} // namespace cappedweather
