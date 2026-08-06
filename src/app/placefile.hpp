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

} // namespace cappedweather
