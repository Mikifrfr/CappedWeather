#include "app/weather_overlay_source.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <cmath>

namespace cappedweather {

namespace {
constexpr auto kAlertsUrl = "https://api.weather.gov/alerts/active?status=actual&message_type=alert";
constexpr auto kTornadoReportsUrl = "https://www.spc.noaa.gov/climo/reports/today_torn.csv";
constexpr auto kHailReportsUrl = "https://www.spc.noaa.gov/climo/reports/today_hail.csv";
constexpr auto kWindReportsUrl = "https://www.spc.noaa.gov/climo/reports/today_wind.csv";

QStringList csvFields(const QString& line) {
    QStringList fields;
    QString field;
    bool quoted = false;
    for (int index = 0; index < line.size(); ++index) {
        const auto ch = line.at(index);
        if (ch == QLatin1Char('"')) {
            if (quoted && index + 1 < line.size() && line.at(index + 1) == QLatin1Char('"')) {
                field += ch;
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (ch == QLatin1Char(',') && !quoted) {
            fields.push_back(field.trimmed());
            field.clear();
        } else {
            field += ch;
        }
    }
    fields.push_back(field.trimmed());
    return fields;
}

QPolygonF coordinateRing(const QJsonArray& ring) {
    QPolygonF polygon;
    polygon.reserve(ring.size());
    for (const auto& coordinateValue : ring) {
        const auto coordinate = coordinateValue.toArray();
        if (coordinate.size() >= 2 && coordinate.at(0).isDouble() && coordinate.at(1).isDouble()) {
            polygon.push_back({coordinate.at(0).toDouble(), coordinate.at(1).toDouble()});
        }
    }
    return polygon;
}

void appendGeometry(const QJsonObject& geometry, QVector<QPolygonF>& polygons) {
    const auto type = geometry.value(QStringLiteral("type")).toString();
    const auto coordinates = geometry.value(QStringLiteral("coordinates")).toArray();
    if (type == QStringLiteral("Polygon")) {
        if (!coordinates.isEmpty()) {
            auto polygon = coordinateRing(coordinates.first().toArray());
            if (polygon.size() >= 3) polygons.push_back(std::move(polygon));
        }
    } else if (type == QStringLiteral("MultiPolygon")) {
        for (const auto& polygonValue : coordinates) {
            const auto rings = polygonValue.toArray();
            if (rings.isEmpty()) continue;
            auto polygon = coordinateRing(rings.first().toArray());
            if (polygon.size() >= 3) polygons.push_back(std::move(polygon));
        }
    }
}
}

WeatherOverlaySource::WeatherOverlaySource(QObject* parent)
    : QObject(parent) {
    connect(&network_, &QNetworkAccessManager::finished, this, &WeatherOverlaySource::requestFinished);
}

QVector<WeatherWarning> WeatherOverlaySource::parseAlerts(const QByteArray& data, QString* error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Invalid NWS alert GeoJSON: %1").arg(parseError.errorString());
        return {};
    }

    QVector<WeatherWarning> warnings;
    for (const auto& value : document.object().value(QStringLiteral("features")).toArray()) {
        const auto feature = value.toObject();
        const auto geometryValue = feature.value(QStringLiteral("geometry"));
        if (!geometryValue.isObject()) continue; // Zone-only alerts cannot be drawn as polygons.
        const auto properties = feature.value(QStringLiteral("properties")).toObject();
        WeatherWarning warning;
        warning.event = properties.value(QStringLiteral("event")).toString();
        warning.headline = properties.value(QStringLiteral("headline")).toString();
        warning.severity = properties.value(QStringLiteral("severity")).toString();
        warning.expires = QDateTime::fromString(properties.value(QStringLiteral("expires")).toString(), Qt::ISODate);
        appendGeometry(geometryValue.toObject(), warning.polygons);
        if (!warning.polygons.isEmpty()) warnings.push_back(std::move(warning));
    }
    if (error) error->clear();
    return warnings;
}

QVector<StormReport> WeatherOverlaySource::parseStormReports(const QByteArray& data,
                                                              const StormReportKind kind,
                                                              QString* error) {
    QVector<StormReport> reports;
    const auto lines = QString::fromUtf8(data).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        if (error) *error = QStringLiteral("Empty SPC storm-report response");
        return reports;
    }

    const auto header = csvFields(lines.first());
    const auto column = [&header](const QString& name) { return header.indexOf(name, 0, Qt::CaseInsensitive); };
    const auto timeColumn = column(QStringLiteral("Time"));
    const auto magnitudeColumn = kind == StormReportKind::Tornado
                                     ? column(QStringLiteral("F_Scale"))
                                     : column(QStringLiteral("Size")) >= 0
                                           ? column(QStringLiteral("Size"))
                                           : column(QStringLiteral("Speed"));
    const auto locationColumn = column(QStringLiteral("Location"));
    const auto stateColumn = column(QStringLiteral("State"));
    const auto latitudeColumn = column(QStringLiteral("Lat"));
    const auto longitudeColumn = column(QStringLiteral("Lon"));
    const auto commentsColumn = column(QStringLiteral("Comments"));
    if (latitudeColumn < 0 || longitudeColumn < 0) {
        if (error) *error = QStringLiteral("SPC report columns Lat/Lon are missing");
        return {};
    }

    for (int row = 1; row < lines.size(); ++row) {
        const auto fields = csvFields(lines.at(row).trimmed());
        const auto get = [&fields](const int index) {
            return index >= 0 && index < fields.size() ? fields.at(index) : QString{};
        };
        bool latitudeOk = false;
        bool longitudeOk = false;
        const auto latitude = get(latitudeColumn).toDouble(&latitudeOk);
        const auto longitude = get(longitudeColumn).toDouble(&longitudeOk);
        if (!latitudeOk || !longitudeOk || !std::isfinite(latitude) || !std::isfinite(longitude)) continue;
        reports.push_back({kind, {longitude, latitude}, get(timeColumn), get(magnitudeColumn),
                           get(locationColumn), get(stateColumn), get(commentsColumn)});
    }
    if (error) error->clear();
    return reports;
}

void WeatherOverlaySource::refresh() {
    if (pending_ != 0) return;
    warnings_.clear();
    reports_.clear();
    failures_ = 0;
    firstError_.clear();
    pending_ = 4;
    request(QUrl(QString::fromUtf8(kAlertsUrl)), RequestKind::Alerts, QByteArrayLiteral("application/geo+json"));
    request(QUrl(QString::fromUtf8(kTornadoReportsUrl)), RequestKind::TornadoReports, QByteArrayLiteral("text/csv"));
    request(QUrl(QString::fromUtf8(kHailReportsUrl)), RequestKind::HailReports, QByteArrayLiteral("text/csv"));
    request(QUrl(QString::fromUtf8(kWindReportsUrl)), RequestKind::WindReports, QByteArrayLiteral("text/csv"));
    emit statusChanged(QStringLiteral("updating NWS warnings and SPC reports"));
}

void WeatherOverlaySource::request(const QUrl& url, const RequestKind kind, const QByteArray& accept) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("CappedWeather/0.1 radar viewer contact@example.invalid"));
    if (!accept.isEmpty()) request.setRawHeader(QByteArrayLiteral("Accept"), accept);
    auto* reply = network_.get(request);
    requests_.insert(reply, kind);
}

void WeatherOverlaySource::requestFinished(QNetworkReply* reply) {
    const auto kind = requests_.take(reply);
    if (reply->error() != QNetworkReply::NoError) {
        ++failures_;
        if (firstError_.isEmpty()) firstError_ = reply->errorString();
    } else {
        const auto data = reply->readAll();
        QString error;
        if (kind == RequestKind::Alerts) {
            warnings_ = parseAlerts(data, &error);
        } else {
            const auto reportKind = kind == RequestKind::TornadoReports
                                        ? StormReportKind::Tornado
                                        : kind == RequestKind::HailReports ? StormReportKind::Hail
                                                                           : StormReportKind::Wind;
            reports_.append(parseStormReports(data, reportKind, &error));
        }
        if (!error.isEmpty()) {
            ++failures_;
            if (firstError_.isEmpty()) firstError_ = error;
        }
    }
    reply->deleteLater();
    --pending_;
    publishIfComplete();
}

void WeatherOverlaySource::publishIfComplete() {
    if (pending_ != 0) return;
    emit overlaysReady(warnings_, reports_, QDateTime::currentDateTimeUtc());
    if (failures_ == 0) {
        emit statusChanged(QStringLiteral("%1 polygon warnings | %2 storm reports")
                               .arg(warnings_.size()).arg(reports_.size()));
    } else {
        emit statusChanged(QStringLiteral("overlay update partial (%1 of 4 failed): %2")
                               .arg(failures_).arg(firstError_));
    }
}

} // namespace cappedweather
