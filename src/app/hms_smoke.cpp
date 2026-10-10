#include "app/hms_smoke.hpp"

#include "app/http_client.hpp"

#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>

namespace cappedweather {
namespace {

constexpr char kHmsBase[] = "https://satepsanone.nesdis.noaa.gov/pub/FIRE/web/HMS/Smoke_Polygons/KML/";

HmsSmokeDensity densityForStyle(const QString& styleUrl, const QString& description) {
    const auto text = (styleUrl + QStringLiteral(" ") + description).toLower();
    if (text.contains(QStringLiteral("heavy"))) return HmsSmokeDensity::Heavy;
    if (text.contains(QStringLiteral("medium")) || text.contains(QStringLiteral("moderate")))
        return HmsSmokeDensity::Medium;
    return HmsSmokeDensity::Light;
}

QString fieldAfter(const QString& text, const QString& marker) {
    const int at = text.indexOf(marker, 0, Qt::CaseInsensitive);
    if (at < 0) return {};
    const auto rest = text.mid(at + marker.size()).trimmed();
    static const QRegularExpression end(QStringLiteral("(<|\\n)"));
    const auto match = end.match(rest);
    return (match.hasMatch() ? rest.left(match.capturedStart()) : rest).trimmed();
}

}  // namespace

QUrl hmsSmokeUrl(const QDate date) {
    if (!date.isValid()) return {};
    return QUrl(QString::fromLatin1(kHmsBase) +
                QStringLiteral("%1/%2/hms_smoke%3%4%5.kml")
                    .arg(date.year())
                    .arg(date.month(), 2, 10, QLatin1Char('0'))
                    .arg(date.year())
                    .arg(date.month(), 2, 10, QLatin1Char('0'))
                    .arg(date.day(), 2, 10, QLatin1Char('0')));
}

QString hmsSmokeDensityLabel(const HmsSmokeDensity density) {
    switch (density) {
    case HmsSmokeDensity::Heavy: return QStringLiteral("heavy");
    case HmsSmokeDensity::Medium: return QStringLiteral("medium");
    case HmsSmokeDensity::Light:
    default: return QStringLiteral("light");
    }
}

QVector<HmsSmokePlume> parseHmsSmokeKml(const QByteArray& data, QString* error) {
    QXmlStreamReader reader(data);
    QVector<HmsSmokePlume> plumes;
    bool inPlacemark = false;
    QString description;
    QString styleUrl;
    QVector<QPointF> ring;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const auto name = reader.name().toString();
            if (name == QStringLiteral("Placemark")) {
                inPlacemark = true;
                description.clear();
                styleUrl.clear();
                ring.clear();
            } else if (inPlacemark && name == QStringLiteral("description")) {
                description = reader.readElementText();
            } else if (inPlacemark && name == QStringLiteral("styleUrl")) {
                styleUrl = reader.readElementText();
            } else if (inPlacemark && name == QStringLiteral("coordinates")) {
                const auto text = reader.readElementText();
                for (const auto& triple :
                     text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
                    const auto parts = triple.split(QLatin1Char(','));
                    if (parts.size() < 2) continue;
                    bool okLon = false, okLat = false;
                    const double lon = parts.at(0).toDouble(&okLon);
                    const double lat = parts.at(1).toDouble(&okLat);
                    if (!okLon || !okLat || lon < -180.0 || lon > 180.0 || lat < -90.0 ||
                        lat > 90.0)
                        continue;
                    ring.push_back(QPointF(lon, lat));
                }
            }
        } else if (reader.isEndElement() && reader.name() == QStringLiteral("Placemark")) {
            inPlacemark = false;
            // ScreenOverlays and legend entries are Placemarks without
            // polygons: only drawable rings become plumes.
            if (ring.size() >= 4) {
                HmsSmokePlume plume;
                plume.density = densityForStyle(styleUrl, description);
                plume.satellite = fieldAfter(description, QStringLiteral("Satellite:"));
                plume.startText = fieldAfter(description, QStringLiteral("Start Time:"));
                plume.endText = fieldAfter(description, QStringLiteral("End Time:"));
                plume.ring = ring;
                plumes.push_back(std::move(plume));
            }
        }
    }
    if (reader.hasError()) {
        if (error)
            *error = QStringLiteral("Invalid HMS smoke KML: %1").arg(reader.errorString());
        return {};
    }
    if (error) error->clear();
    return plumes;
}

HmsSmokeSource::HmsSmokeSource(QObject* parent) : QObject(parent) {}

void HmsSmokeSource::refresh() {
    if (!enabled_ || pending_) return;
    pending_ = true;
    requestDay(QDateTime::currentDateTimeUtc().date(), true);
}

void HmsSmokeSource::setEnabled(const bool enabled) {
    enabled_ = enabled;
}

void HmsSmokeSource::requestDay(const QDate date, const bool fallbackLeft) {
    const QUrl url = hmsSmokeUrl(date);
    if (!url.isValid()) {
        pending_ = false;
        emit statusChanged(QStringLiteral("HMS smoke: bad date"));
        return;
    }
    auto* reply = network_.get(
        makeRequest(url, QStringLiteral("application/xml"), QStringLiteral(" HMS smoke client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, date, fallbackLeft] {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const int code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool httpOk = ok && (code == 0 || (code >= 200 && code < 300));
        // Today's analysis lands mid-morning UTC; a 404 before then falls
        // back to yesterday instead of reading as a broken feed.
        if (!httpOk && fallbackLeft && code == 404) {
            requestDay(date.addDays(-1), false);
            return;
        }
        if (!httpOk) {
            pending_ = false;
            emit statusChanged(QStringLiteral("HMS smoke: %1")
                                   .arg(ok ? QStringLiteral("HTTP %1").arg(code)
                                           : reply->errorString()));
            return;
        }
        QString error;
        auto plumes = parseHmsSmokeKml(reply->readAll(), &error);
        pending_ = false;
        if (!error.isEmpty()) {
            emit statusChanged(QStringLiteral("HMS smoke: %1").arg(error));
            return;
        }
        plumes_ = std::move(plumes);
        emit smokeReady(plumes_);
        emit statusChanged(plumes_.isEmpty()
                               ? QStringLiteral("HMS smoke: none today")
                               : QStringLiteral("HMS smoke: %1 plumes").arg(plumes_.size()));
    });
}

}  // namespace cappedweather
