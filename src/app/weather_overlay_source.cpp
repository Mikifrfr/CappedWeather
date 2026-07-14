#include "app/weather_overlay_source.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace cappedweather {

namespace {
constexpr auto kAlertsUrl = "https://api.weather.gov/alerts/active?status=actual&message_type=alert";
constexpr auto kTornadoReportsUrl = "https://www.spc.noaa.gov/climo/reports/today_torn.csv";
constexpr auto kHailReportsUrl = "https://www.spc.noaa.gov/climo/reports/today_hail.csv";
constexpr auto kWindReportsUrl = "https://www.spc.noaa.gov/climo/reports/today_wind.csv";
constexpr auto kYesterdayTornadoReportsUrl = "https://www.spc.noaa.gov/climo/reports/yesterday_torn.csv";
constexpr auto kYesterdayHailReportsUrl = "https://www.spc.noaa.gov/climo/reports/yesterday_hail.csv";
constexpr auto kYesterdayWindReportsUrl = "https://www.spc.noaa.gov/climo/reports/yesterday_wind.csv";

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

void appendPolygon(const QJsonArray& rings, QVector<WarningPolygon>& polygons) {
    if (rings.isEmpty()) return;
    WarningPolygon polygon;
    polygon.exterior = coordinateRing(rings.first().toArray());
    if (polygon.exterior.size() < 3) return;
    for (qsizetype index = 1; index < rings.size(); ++index) {
        auto hole = coordinateRing(rings.at(index).toArray());
        if (hole.size() >= 3) polygon.holes.push_back(std::move(hole));
    }
    polygons.push_back(std::move(polygon));
}

void appendGeometry(const QJsonObject& geometry, QVector<WarningPolygon>& polygons) {
    const auto type = geometry.value(QStringLiteral("type")).toString();
    const auto coordinates = geometry.value(QStringLiteral("coordinates")).toArray();
    if (type == QStringLiteral("Polygon")) {
        appendPolygon(coordinates, polygons);
    } else if (type == QStringLiteral("MultiPolygon")) {
        for (const auto& polygonValue : coordinates) {
            const auto rings = polygonValue.toArray();
            appendPolygon(rings, polygons);
        }
    }
}

QVector<WarningPolygon> parseZoneGeometryDocument(const QByteArray& data, QString* error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Invalid NWS zone GeoJSON: %1").arg(parseError.errorString());
        return {};
    }
    QVector<WarningPolygon> polygons;
    const auto root = document.object();
    const auto geometry = root.value(QStringLiteral("geometry"));
    if (geometry.isObject()) appendGeometry(geometry.toObject(), polygons);
    if (polygons.isEmpty()) {
        const auto features = root.value(QStringLiteral("features")).toArray();
        const auto featureGeometry = (features.isEmpty() ? QJsonObject{} : features.first().toObject())
                                         .value(QStringLiteral("geometry"));
        if (featureGeometry.isObject()) appendGeometry(featureGeometry.toObject(), polygons);
    }
    if (polygons.isEmpty() && error) *error = QStringLiteral("NWS zone response has no polygon geometry");
    else if (error) error->clear();
    return polygons;
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
        const auto properties = feature.value(QStringLiteral("properties")).toObject();
        WeatherWarning warning;
        warning.event = properties.value(QStringLiteral("event")).toString();
        warning.headline = properties.value(QStringLiteral("headline")).toString();
        warning.severity = properties.value(QStringLiteral("severity")).toString();
        warning.senderName = properties.value(QStringLiteral("senderName")).toString();
        warning.description = properties.value(QStringLiteral("description")).toString();
        warning.instruction = properties.value(QStringLiteral("instruction")).toString();
        warning.onset = QDateTime::fromString(properties.value(QStringLiteral("onset")).toString(), Qt::ISODate);
        warning.expires = QDateTime::fromString(properties.value(QStringLiteral("expires")).toString(), Qt::ISODate);
        for (const auto& zone : properties.value(QStringLiteral("affectedZones")).toArray()) {
            if (zone.isString()) warning.affectedZones.push_back(zone.toString());
        }
        if (geometryValue.isObject()) appendGeometry(geometryValue.toObject(), warning.polygons);
        if (!warning.polygons.isEmpty() || !warning.affectedZones.isEmpty()) warnings.push_back(std::move(warning));
    }
    if (error) error->clear();
    return warnings;
}

QVector<WarningPolygon> WeatherOverlaySource::parseZone(const QByteArray& data, QString* error) {
    return parseZoneGeometryDocument(data, error);
}

QVector<StormReport> WeatherOverlaySource::parseStormReports(const QByteArray& data,
                                                              const StormReportKind kind,
                                                              QString* error,
                                                              QDate convectiveDate) {
    QVector<StormReport> reports;
    const auto lines = QString::fromUtf8(data).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        // SPC legitimately returns an empty response when no reports exist yet.
        if (error) error->clear();
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
        const auto time = get(timeColumn).rightJustified(4, QLatin1Char('0'));
        bool timeOk = false;
        const auto hhmm = time.toInt(&timeOk);
        if (!convectiveDate.isValid()) {
            const auto now = QDateTime::currentDateTimeUtc();
            convectiveDate = now.time().hour() < 12 ? now.date().addDays(-1) : now.date();
        }
        QDateTime occurredAt;
        if (timeOk && hhmm >= 0 && hhmm <= 2359 && hhmm % 100 < 60) {
            auto calendarDate = convectiveDate;
            if (hhmm / 100 < 12) calendarDate = calendarDate.addDays(1);
            occurredAt = QDateTime(calendarDate, QTime(hhmm / 100, hhmm % 100), QTimeZone::UTC);
        }
        reports.push_back({kind, {longitude, latitude}, time, occurredAt, get(magnitudeColumn),
                           get(locationColumn), get(stateColumn), get(commentsColumn)});
    }
    if (error) error->clear();
    return reports;
}

void WeatherOverlaySource::refresh() {
    if (pending_ != 0) return;
    failures_ = 0;
    firstError_.clear();
    pending_ = totalRequests_ = 7;
    zoneWarningIndexes_.clear();
    const auto now = QDateTime::currentDateTimeUtc();
    const auto today = now.time().hour() < 12 ? now.date().addDays(-1) : now.date();
    request(QUrl(QString::fromUtf8(kAlertsUrl)), {RequestKind::Alerts}, QByteArrayLiteral("application/geo+json"));
    const std::array<const char*, 3> todayUrls{kTornadoReportsUrl, kHailReportsUrl, kWindReportsUrl};
    const std::array<const char*, 3> yesterdayUrls{kYesterdayTornadoReportsUrl, kYesterdayHailReportsUrl,
                                                   kYesterdayWindReportsUrl};
    for (int index = 0; index < 3; ++index) {
        const auto kind = static_cast<StormReportKind>(index);
        request(QUrl(QString::fromUtf8(todayUrls.at(index))), {RequestKind::Reports, index, kind, today},
                QByteArrayLiteral("text/csv"));
        request(QUrl(QString::fromUtf8(yesterdayUrls.at(index))),
                {RequestKind::Reports, index + 3, kind, today.addDays(-1)}, QByteArrayLiteral("text/csv"));
    }
    emit statusChanged(QStringLiteral("updating NWS warnings and SPC reports"));
}

void WeatherOverlaySource::request(const QUrl& url, RequestContext context, const QByteArray& accept) {
    QNetworkRequest request(url);
    const auto contact = QSettings().value(QStringLiteral("network/contact")).toString().trimmed();
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      contact.isEmpty() ? QStringLiteral("CappedWeather/0.3 operational radar viewer")
                                        : QStringLiteral("CappedWeather/0.3 operational radar viewer (%1)").arg(contact));
    request.setTransferTimeout(std::clamp(QSettings().value(QStringLiteral("network/timeoutSeconds"), 30).toInt(),
                                          5, 120) * 1000);
    if (!accept.isEmpty()) request.setRawHeader(QByteArrayLiteral("Accept"), accept);
    auto* reply = network_.get(request);
    requests_.insert(reply, std::move(context));
}

void WeatherOverlaySource::requestFinished(QNetworkReply* reply) {
    const auto context = requests_.take(reply);
    if (reply->error() != QNetworkReply::NoError) {
        ++failures_;
        if (firstError_.isEmpty()) firstError_ = reply->errorString();
    } else {
        const auto data = reply->readAll();
        QString error;
        if (context.kind == RequestKind::Alerts) {
            auto parsed = parseAlerts(data, &error);
            if (error.isEmpty()) {
                warnings_ = std::move(parsed);
                for (int warningIndex = 0; warningIndex < warnings_.size(); ++warningIndex) {
                    if (!warnings_.at(warningIndex).polygons.isEmpty()) continue;
                    for (const auto& zoneUrl : warnings_.at(warningIndex).affectedZones) {
                        if (zoneCache_.contains(zoneUrl)) {
                            warnings_[warningIndex].polygons.append(zoneCache_.value(zoneUrl));
                        } else {
                            zoneWarningIndexes_[zoneUrl].push_back(warningIndex);
                        }
                    }
                }
                for (auto it = zoneWarningIndexes_.cbegin(); it != zoneWarningIndexes_.cend(); ++it) {
                    ++pending_;
                    ++totalRequests_;
                    request(QUrl(it.key()), {RequestKind::Zone, -1, StormReportKind::Wind, {}, it.key()},
                            QByteArrayLiteral("application/geo+json"));
                }
            }
        } else if (context.kind == RequestKind::Reports) {
            auto parsed = parseStormReports(data, context.reportKind, &error, context.convectiveDate);
            if (error.isEmpty() && context.reportSet >= 0)
                reportSets_.at(static_cast<std::size_t>(context.reportSet)) = std::move(parsed);
        } else if (context.kind == RequestKind::Zone) {
            auto polygons = parseZone(data, &error);
            if (error.isEmpty()) {
                zoneCache_.insert(context.zoneUrl, polygons);
                for (const auto index : zoneWarningIndexes_.value(context.zoneUrl)) {
                    if (index >= 0 && index < warnings_.size()) warnings_[index].polygons.append(polygons);
                }
            }
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
    reports_.clear();
    const auto cutoff = QDateTime::currentDateTimeUtc().addSecs(-24 * 60 * 60);
    for (const auto& set : reportSets_) {
        for (const auto& report : set) {
            if (!report.occurredAt.isValid() || report.occurredAt >= cutoff) reports_.push_back(report);
        }
    }
    emit overlaysReady(warnings_, reports_, QDateTime::currentDateTimeUtc());
    if (failures_ == 0) {
        const auto drawable = std::count_if(warnings_.cbegin(), warnings_.cend(),
                                            [](const auto& warning) { return !warning.polygons.isEmpty(); });
        emit statusChanged(QStringLiteral("%1 drawable warnings | %2 reports (24 h)")
                               .arg(drawable).arg(reports_.size()));
    } else {
        emit statusChanged(QStringLiteral("overlay update partial (%1 of %2 requests failed): %3")
                               .arg(failures_).arg(totalRequests_).arg(firstError_));
    }
}

} // namespace cappedweather
