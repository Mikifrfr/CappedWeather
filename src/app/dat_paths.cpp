#include "app/dat_paths.hpp"

#include "app/http_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTimeZone>

namespace cappedweather {
namespace {

constexpr char kDatQuery[] =
    "https://services.dat.noaa.gov/arcgis/rest/services/nws_damageassessmenttoolkit/"
    "DamageViewer/MapServer/1/query?where=stormdate%3E%1&outFields=objectid%2Cevent_id%2Cefscale%"
    "2Cstormdate%2Cstartlat%2Cstartlon%2Cendlat%2Cendlon&returnGeometry=true&outSR=4326&"
    "orderByFields=stormdate%20DESC&resultRecordCount=500&f=geojson";
// 90 days of surveys: recent enough for chase context, small enough to
// stay far under the 2000-record cap.
constexpr qint64 kLookbackDays = 90;

}  // namespace

void datPathColors(const QString& efScale, QString& stroke, QString& fill) {
    const auto upper = efScale.toUpper().trimmed();
    if (upper == QStringLiteral("EF5")) {
        stroke = QStringLiteral("#FFFFFF");
        fill = QStringLiteral("#FF00FF");
    } else if (upper == QStringLiteral("EF4")) {
        stroke = QStringLiteral("#C084FC");
        fill = QStringLiteral("#C084FC");
    } else if (upper == QStringLiteral("EF3")) {
        stroke = QStringLiteral("#FF3B4D");
        fill = QStringLiteral("#FF3B4D");
    } else if (upper == QStringLiteral("EF2")) {
        stroke = QStringLiteral("#FF9F43");
        fill = QStringLiteral("#FF9F43");
    } else if (upper == QStringLiteral("EF1")) {
        stroke = QStringLiteral("#FFFE00");
        fill = QStringLiteral("#FFFE00");
    } else {
        stroke = QStringLiteral("#53D769");
        fill = QStringLiteral("#53D769");
    }
}

QVector<DatPath> parseDatPaths(const QByteArray& data, QString* error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid DAT paths GeoJSON: %1").arg(parseError.errorString());
        return {};
    }
    QVector<DatPath> paths;
    for (const auto& value : document.object().value(QStringLiteral("features")).toArray()) {
        const auto feature = value.toObject();
        const auto geometry = feature.value(QStringLiteral("geometry")).toObject();
        if (geometry.value(QStringLiteral("type")).toString() != QStringLiteral("LineString"))
            continue;
        QVector<QPointF> line;
        for (const auto& point : geometry.value(QStringLiteral("coordinates")).toArray()) {
            const auto pair = point.toArray();
            if (pair.size() < 2) continue;
            const double lon = pair.at(0).toDouble();
            const double lat = pair.at(1).toDouble();
            if (!std::isfinite(lon) || !std::isfinite(lat) || lon < -180.0 || lon > 180.0 ||
                lat < -90.0 || lat > 90.0)
                continue;
            line.push_back(QPointF(lon, lat));
        }
        if (line.size() < 2) continue;
        const auto props = feature.value(QStringLiteral("properties")).toObject();
        DatPath path;
        path.objectId = props.value(QStringLiteral("objectid")).toInt();
        path.efScale = props.value(QStringLiteral("efscale")).toString().trimmed();
        const auto stormMs = static_cast<qint64>(props.value(QStringLiteral("stormdate")).toDouble());
        if (stormMs > 0) path.stormDate = QDateTime::fromMSecsSinceEpoch(stormMs, QTimeZone::UTC);
        path.startLatitude = props.value(QStringLiteral("startlat")).toDouble();
        path.startLongitude = props.value(QStringLiteral("startlon")).toDouble();
        path.endLatitude = props.value(QStringLiteral("endlat")).toDouble();
        path.endLongitude = props.value(QStringLiteral("endlon")).toDouble();
        path.line = std::move(line);
        paths.push_back(std::move(path));
    }
    if (error) error->clear();
    return paths;
}

DatPathSource::DatPathSource(QObject* parent) : QObject(parent) {}

void DatPathSource::refresh() {
    if (!enabled_ || pending_) return;
    pending_ = true;
    paths_.clear();
    const auto sinceMs =
        QDateTime::currentDateTimeUtc().addDays(-kLookbackDays).toMSecsSinceEpoch();
    const QUrl url(QString::fromLatin1(kDatQuery).arg(sinceMs));
    auto* reply = network_.get(
        makeRequest(url, QStringLiteral("application/geo+json"), QStringLiteral(" DAT survey client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        pending_ = false;
        if (reply->error() != QNetworkReply::NoError) {
            emit statusChanged(
                QStringLiteral("DAT surveys: feed failed (%1)").arg(reply->errorString()));
            return;
        }
        QString error;
        paths_ = parseDatPaths(reply->readAll(), &error);
        if (!error.isEmpty()) {
            emit statusChanged(QStringLiteral("DAT surveys: %1").arg(error));
            return;
        }
        emit pathsReady(paths_);
        emit statusChanged(paths_.isEmpty() ? QStringLiteral("DAT surveys: none in 90 days")
                                            : QStringLiteral("DAT surveys: %1 paths")
                                                  .arg(paths_.size()));
    });
}

void DatPathSource::setEnabled(const bool enabled) {
    enabled_ = enabled;
}

}  // namespace cappedweather
