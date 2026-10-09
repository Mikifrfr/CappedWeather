#include "app/metar_source.hpp"

#include "app/http_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTimeZone>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cappedweather {
namespace {

// aviationweather.gov JSON API (the legacy text dataserver is retired).
// stationinfo bbox order is minlat,minlon,maxlat,maxlon (lat first).
constexpr char kStationInfoBase[] = "https://aviationweather.gov/api/data/stationinfo";
constexpr char kMetarBase[] = "https://aviationweather.gov/api/data/metar";
constexpr int kMaxStations = 120;  // priority-sorted cap: majors first
constexpr int kObsHours = 2;  // hourly reports: two windows deduped newest-wins

double jsonDouble(const QJsonObject& object, const char* key) {
    const auto value = object.value(QString::fromLatin1(key));
    if (value.isDouble()) return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok) return parsed;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

int jsonInt(const QJsonObject& object, const char* key, int fallback = -1) {
    const auto value = object.value(QString::fromLatin1(key));
    if (value.isDouble()) return static_cast<int>(std::llround(value.toDouble()));
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().toInt(&ok);
        if (ok) return parsed;
    }
    return fallback;
}

}  // namespace

QVector<MetarStation> parseMetarStations(const QByteArray& data, QString* error) {
    QVector<MetarStation> stations;
    if (error) error->clear();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (error) *error = QStringLiteral("Station catalog is not a JSON array");
        return stations;
    }
    for (const auto& entry : document.array()) {
        if (!entry.isObject()) continue;
        const auto object = entry.toObject();
        MetarStation station;
        station.icaoId = object.value(QStringLiteral("icaoId")).toString();
        if (station.icaoId.isEmpty()) continue;
        const auto latValue = object.value(QStringLiteral("lat"));
        const auto lonValue = object.value(QStringLiteral("lon"));
        if (!latValue.isDouble() || !lonValue.isDouble()) continue;
        station.latitude = latValue.toDouble();
        station.longitude = lonValue.toDouble();
        if (!std::isfinite(station.latitude) || !std::isfinite(station.longitude)) continue;
        if (std::abs(station.latitude) > 90.0 || std::abs(station.longitude) > 180.0) continue;
        station.hasCoords = true;
        station.site = object.value(QStringLiteral("site")).toString();
        station.priority = jsonInt(object, "priority", 9);
        for (const auto& type : object.value(QStringLiteral("siteType")).toArray()) {
            if (type.toString() == QStringLiteral("METAR")) {
                station.hasMetar = true;
                break;
            }
        }
        if (!station.hasMetar) continue;
        stations.push_back(station);
    }
    return stations;
}

QVector<MetarObs> parseMetarObs(const QByteArray& data, QString* error) {
    QVector<MetarObs> obs;
    if (error) error->clear();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        if (error) *error = QStringLiteral("METAR payload is not a JSON array");
        return obs;
    }
    for (const auto& entry : document.array()) {
        if (!entry.isObject()) continue;
        const auto object = entry.toObject();
        MetarObs row;
        row.icaoId = object.value(QStringLiteral("icaoId")).toString();
        if (row.icaoId.isEmpty()) continue;
        const auto latValue = object.value(QStringLiteral("lat"));
        const auto lonValue = object.value(QStringLiteral("lon"));
        if (!latValue.isDouble() || !lonValue.isDouble()) continue;
        row.latitude = latValue.toDouble();
        row.longitude = lonValue.toDouble();
        if (!std::isfinite(row.latitude) || !std::isfinite(row.longitude)) continue;
        row.stationName = object.value(QStringLiteral("name")).toString();
        const long long obsEpoch = object.value(QStringLiteral("obsTime")).toVariant().toLongLong();
        if (obsEpoch > 0)
            row.obsTime = QDateTime::fromSecsSinceEpoch(obsEpoch, QTimeZone::UTC);
        row.tempC = jsonDouble(object, "temp");
        row.dewpointC = jsonDouble(object, "dewp");
        row.windDirectionDegrees = jsonInt(object, "wdir");
        row.windSpeedKnots = jsonInt(object, "wspd");
        row.windGustKnots = jsonInt(object, "wgst");
        row.flightCategory = object.value(QStringLiteral("fltCat")).toString().toUpper();
        // Newest report per station wins (hours=2 returns two hourly obs).
        auto existing = std::find_if(obs.begin(), obs.end(), [&row](const MetarObs& known) {
            return known.icaoId == row.icaoId;
        });
        if (existing == obs.end()) {
            obs.push_back(row);
        } else if (row.obsTime.isValid() &&
                   (!existing->obsTime.isValid() || row.obsTime > existing->obsTime)) {
            *existing = row;
        }
    }
    return obs;
}

QColor metarCategoryColor(const QString& flightCategory) {
    const auto category = flightCategory.toUpper();
    if (category == QStringLiteral("VFR")) return QColor(QStringLiteral("#53d769"));
    if (category == QStringLiteral("MVFR")) return QColor(QStringLiteral("#44a7ff"));
    if (category == QStringLiteral("IFR")) return QColor(QStringLiteral("#ff9f43"));
    if (category == QStringLiteral("LIFR")) return QColor(QStringLiteral("#ff3b4d"));
    return QColor(QStringLiteral("#9aa0a6"));
}

MetarSource::MetarSource(QObject* parent) : QObject(parent) {}

void MetarSource::setEnabled(const bool enabled) {
    enabled_ = enabled;
}

void MetarSource::setRegion(const double latitude, const double longitude, const double halfDegrees) {
    if (!std::isfinite(latitude) || !std::isfinite(longitude)) return;
    regionLatitude_ = std::clamp(latitude, -90.0, 90.0);
    regionLongitude_ = std::clamp(longitude, -180.0, 180.0);
    regionHalfDegrees_ = std::clamp(halfDegrees, 1.0, 15.0);
}

void MetarSource::refresh() {
    if (!enabled_ || pending_) return;
    pending_ = true;
    fetchStations();
}

void MetarSource::fetchStations() {
    QUrl url(QString::fromLatin1(kStationInfoBase));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("bbox"),
                       QStringLiteral("%1,%2,%3,%4")
                           .arg(regionLatitude_ - regionHalfDegrees_, 0, 'f', 2)
                           .arg(regionLongitude_ - regionHalfDegrees_, 0, 'f', 2)
                           .arg(regionLatitude_ + regionHalfDegrees_, 0, 'f', 2)
                           .arg(regionLongitude_ + regionHalfDegrees_, 0, 'f', 2));
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    url.setQuery(query);
    auto* reply = network_.get(makeRequest(url, QStringLiteral("application/json"),
                                           QStringLiteral(" METAR station client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || (code != 0 && (code < 200 || code >= 300))) {
            pending_ = false;
            // Fail closed: keep painting last-known-good, say so.
            emit statusChanged(QStringLiteral("METAR stations: feed failed (%1), keeping last obs")
                                   .arg(reply->error() != QNetworkReply::NoError
                                            ? reply->errorString()
                                            : QStringLiteral("HTTP %1").arg(code)));
            return;
        }
        QString error;
        auto stations = parseMetarStations(reply->readAll(), &error);
        if (!error.isEmpty()) {
            pending_ = false;
            emit statusChanged(QStringLiteral("METAR stations: %1, keeping last obs").arg(error));
            return;
        }
        std::sort(stations.begin(), stations.end(), [](const MetarStation& a, const MetarStation& b) {
            return a.priority < b.priority;
        });
        if (stations.size() > kMaxStations) stations.resize(kMaxStations);
        stations_ = std::move(stations);
        fetchObs();
    });
}

void MetarSource::fetchObs() {
    if (stations_.isEmpty()) {
        pending_ = false;
        metars_.clear();
        emit metarsReady(metars_);
        emit statusChanged(QStringLiteral("METAR: no stations in this region"));
        return;
    }
    QStringList ids;
    ids.reserve(stations_.size());
    for (const auto& station : stations_) ids.push_back(station.icaoId);
    QUrl url(QString::fromLatin1(kMetarBase));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ids"), ids.join(QLatin1Char(',')));
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    query.addQueryItem(QStringLiteral("hours"), QString::number(kObsHours));
    url.setQuery(query);
    auto* reply = network_.get(makeRequest(url, QStringLiteral("application/json"),
                                           QStringLiteral(" METAR obs client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        pending_ = false;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || (code != 0 && (code < 200 || code >= 300))) {
            emit statusChanged(QStringLiteral("METAR obs: feed failed (%1), keeping last obs")
                                   .arg(reply->error() != QNetworkReply::NoError
                                            ? reply->errorString()
                                            : QStringLiteral("HTTP %1").arg(code)));
            return;
        }
        QString error;
        const auto obs = parseMetarObs(reply->readAll(), &error);
        if (!error.isEmpty()) {
            emit statusChanged(QStringLiteral("METAR obs: %1, keeping last obs").arg(error));
            return;
        }
        metars_ = obs;
        emit metarsReady(metars_);
        emit statusChanged(QStringLiteral("METAR: %1 stations").arg(metars_.size()));
    });
}

}  // namespace cappedweather
