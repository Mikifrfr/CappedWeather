#include "app/wpc_ero.hpp"

#include "app/http_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTimeZone>

#include <algorithm>

namespace cappedweather {
namespace {

constexpr char kWpcBase[] =
    "https://mapservices.weather.noaa.gov/vector/rest/services/hazards/wpc_precip_hazards/MapServer/";
constexpr char kWpcQuery[] =
    "/query?where=1%3D1&outFields=objectid%2Coutlook%2Cdn%2Cvalid_time%2Cissue_time%2Cstart_time%"
    "2Cend_time&returnGeometry=true&outSR=4326&f=geojson";

// Official WPC ERO renderer colors, sampled live from the MapServer
// drawingInfo (dn1 green .. dn4 pink). Pinned here, not fetched, so the
// map never silently restyles when the service does.
void eroColors(const int dn, QString& fill, QString& stroke) {
    switch (dn) {
    case 4:
        fill = QStringLiteral("#FF69C5");
        stroke = QStringLiteral("#FF00FF");
        break;
    case 3:
        fill = QStringLiteral("#F50000");
        stroke = QStringLiteral("#8A0000");
        break;
    case 2:
        fill = QStringLiteral("#FFFE00");
        stroke = QStringLiteral("#E69800");
        break;
    case 1:
    default:
        fill = QStringLiteral("#38A800");
        stroke = QStringLiteral("#00734C");
        break;
    }
}

QString eroShortLabel(const QString& outlook) {
    const auto upper = outlook.toUpper();
    if (upper.contains(QStringLiteral("HIGH"))) return QStringLiteral("HIGH");
    if (upper.contains(QStringLiteral("MODERATE"))) return QStringLiteral("MDT");
    if (upper.contains(QStringLiteral("SLIGHT"))) return QStringLiteral("SLGT");
    return QStringLiteral("MRGL");
}

QDateTime parseWpcTime(const QString& text) {
    if (text.isEmpty()) return {};
    // "2026-10-10 12:00:00" UTC.
    const auto moment =
        QDateTime::fromString(text.trimmed(), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!moment.isValid()) return {};
    return QDateTime(moment.date(), moment.time(), QTimeZone::UTC);
}

}  // namespace

QVector<SpcOutlookFeature> parseWpcEroGeoJson(const QByteArray& data, const int day,
                                             QString* error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid WPC outlook GeoJSON: %1").arg(parseError.errorString());
        return {};
    }
    QVector<SpcOutlookFeature> features;
    for (const auto& value : document.object().value(QStringLiteral("features")).toArray()) {
        const auto feature = value.toObject();
        const auto geometry = feature.value(QStringLiteral("geometry")).toObject();
        const auto type = geometry.value(QStringLiteral("type")).toString();
        const auto coords = geometry.value(QStringLiteral("coordinates")).toArray();
        std::vector<QJsonArray> polygons;
        if (type == QStringLiteral("Polygon")) {
            polygons.push_back(coords);
        } else if (type == QStringLiteral("MultiPolygon")) {
            for (const auto& poly : coords) polygons.push_back(poly.toArray());
        } else {
            continue;
        }
        const auto props = feature.value(QStringLiteral("properties")).toObject();
        SpcOutlookFeature out;
        out.day = day;
        out.fireKind = QStringLiteral("ERO");
        out.label2 = props.value(QStringLiteral("outlook")).toString();
        out.dn = props.value(QStringLiteral("dn")).toInt();
        out.label = eroShortLabel(out.label2);
        out.valid = parseWpcTime(props.value(QStringLiteral("start_time")).toString());
        out.expire = parseWpcTime(props.value(QStringLiteral("end_time")).toString());
        out.issue = parseWpcTime(props.value(QStringLiteral("issue_time")).toString());
        out.forecaster = QStringLiteral("WPC");
        eroColors(out.dn, out.fill, out.stroke);
        if (out.label2.isEmpty() || out.dn <= 0) continue;
        for (const auto& poly : polygons) {
            if (poly.isEmpty()) continue;
            const auto ring = poly.first().toArray();
            if (ring.size() < 4) continue;
            SpcOutlookRing exterior;
            for (const auto& point : ring) {
                const auto pair = point.toArray();
                if (pair.size() < 2) continue;
                exterior.exterior.push_back(QPointF(pair.at(0).toDouble(), pair.at(1).toDouble()));
            }
            if (exterior.exterior.size() >= 4) out.rings.push_back(std::move(exterior));
        }
        if (!out.rings.empty()) features.push_back(std::move(out));
    }
    if (error) error->clear();
    return features;
}

WpcEroSource::WpcEroSource(QObject* parent) : QObject(parent) {}

void WpcEroSource::refresh() {
    if (!enabled_ || pending_) return;
    pending_ = true;
    outlooks_.clear();
    outlooksError_.clear();
    struct Pending {
        int day = 0;
        QNetworkReply* reply = nullptr;
    };
    auto jobs = std::make_shared<QVector<Pending>>();
    auto finishOne = [this, jobs](int day, QNetworkReply* reply) {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const int code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (ok && (code == 0 || (code >= 200 && code < 300))) {
            QString error;
            const auto features = parseWpcEroGeoJson(reply->readAll(), day, &error);
            if (error.isEmpty()) {
                outlooks_.append(features);
            } else {
                outlooksError_ = error;
            }
        } else if (ok) {
            outlooksError_ = QStringLiteral("Day %1 HTTP %2").arg(day).arg(code);
        } else {
            outlooksError_ = reply->errorString();
        }
        reply->deleteLater();
        jobs->erase(std::remove_if(jobs->begin(), jobs->end(),
                                   [reply](const Pending& p) { return p.reply == reply; }),
                    jobs->end());
        if (jobs->isEmpty()) {
            pending_ = false;
            emit outlooksReady(outlooks_);
            if (outlooksError_.isEmpty())
                emit statusChanged(QStringLiteral("WPC rainfall: %1 areas").arg(outlooks_.size()));
            else
                emit statusChanged(QStringLiteral("WPC rainfall: %1 areas (%2)")
                                       .arg(outlooks_.size())
                                       .arg(outlooksError_));
        }
    };
    for (const int day : {1, 2, 3}) {
        const QUrl url(QString::fromLatin1(kWpcBase) + QString::number(day - 1) +
                       QString::fromLatin1(kWpcQuery));
        auto* reply = network_.get(
            makeRequest(url, QStringLiteral("application/geo+json"), QStringLiteral(" WPC ERO client")));
        jobs->push_back({day, reply});
        connect(reply, &QNetworkReply::finished, this,
                [finishOne, day, reply] { finishOne(day, reply); });
    }
}

void WpcEroSource::setEnabled(const bool enabled) {
    enabled_ = enabled;
}

}  // namespace cappedweather
