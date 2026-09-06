#include "app/weather_overlay_source.hpp"
#include "app/http_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <array>
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

// Meteoalarm legacy Atom feeds (EUMETNET, CC BY 4.0 equivalent + ToS at
// meteoalarm.org). One feed per country; slugs verified live 2026-09-06.
// Covers the OPERA radar footprint so European sessions get alerts through
// the same WeatherWarning pipeline as NWS. Extend here to add countries.
constexpr std::array kMeteoalarmFeeds{
    "france",         "germany",   "netherlands", "belgium",  "finland",  "sweden",
    "norway",         "denmark",   "ireland",     "united-kingdom", "spain", "poland",
    "italy",          "austria",
};

QString meteoalarmFeedUrl(const char* slug) {
    return QStringLiteral("https://feeds.meteoalarm.org/feeds/meteoalarm-legacy-atom-%1")
        .arg(QString::fromLatin1(slug));
}

struct OfficialAlertStyle {
    const char* event;
    int priority;
    const char* hex;
};

// NWS Hazards Map colors, last updated by NWS on 2025-03-10.
// Source: https://www.weather.gov/help-map
constexpr std::array kOfficialAlertStyles{
    OfficialAlertStyle{"Tsunami Warning", 1, "#FD6347"},
    OfficialAlertStyle{"Tornado Warning", 2, "#FF0000"},
    OfficialAlertStyle{"Extreme Wind Warning", 3, "#FF8C00"},
    OfficialAlertStyle{"Severe Thunderstorm Warning", 4, "#FFA500"},
    OfficialAlertStyle{"Flash Flood Warning", 5, "#8B0000"},
    OfficialAlertStyle{"Flash Flood Statement", 6, "#8B0000"},
    OfficialAlertStyle{"Severe Weather Statement", 7, "#00FFFF"},
    OfficialAlertStyle{"Shelter In Place Warning", 8, "#FA8072"},
    OfficialAlertStyle{"Evacuation Immediate", 9, "#7FFF00"},
    OfficialAlertStyle{"Civil Danger Warning", 10, "#FFB6C1"},
    OfficialAlertStyle{"Nuclear Power Plant Warning", 11, "#4B0082"},
    OfficialAlertStyle{"Radiological Hazard Warning", 12, "#4B0082"},
    OfficialAlertStyle{"Hazardous Materials Warning", 13, "#4B0082"},
    OfficialAlertStyle{"Fire Warning", 14, "#A0522D"},
    OfficialAlertStyle{"Civil Emergency Message", 15, "#FFB6C1"},
    OfficialAlertStyle{"Law Enforcement Warning", 16, "#C0C0C0"},
    OfficialAlertStyle{"Storm Surge Warning", 17, "#B524F7"},
    OfficialAlertStyle{"Hurricane Force Wind Warning", 18, "#CD5C5C"},
    OfficialAlertStyle{"Hurricane Warning", 19, "#DC143C"},
    OfficialAlertStyle{"Typhoon Warning", 20, "#DC143C"},
    OfficialAlertStyle{"Special Marine Warning", 21, "#FFA500"},
    OfficialAlertStyle{"Blizzard Warning", 22, "#FF4500"},
    OfficialAlertStyle{"Snow Squall Warning", 23, "#C71585"},
    OfficialAlertStyle{"Ice Storm Warning", 24, "#8B008B"},
    OfficialAlertStyle{"Heavy Freezing Spray Warning", 25, "#00BFFF"},
    OfficialAlertStyle{"Winter Storm Warning", 26, "#FF69B4"},
    OfficialAlertStyle{"Lake Effect Snow Warning", 27, "#008B8B"},
    OfficialAlertStyle{"Dust Storm Warning", 28, "#FFE4C4"},
    OfficialAlertStyle{"Blowing Dust Warning", 29, "#FFE4C4"},
    OfficialAlertStyle{"High Wind Warning", 30, "#DAA520"},
    OfficialAlertStyle{"Tropical Storm Warning", 31, "#B22222"},
    OfficialAlertStyle{"Storm Warning", 32, "#9400D3"},
    OfficialAlertStyle{"Tsunami Advisory", 33, "#D2691E"},
    OfficialAlertStyle{"Tsunami Watch", 34, "#FF00FF"},
    OfficialAlertStyle{"Avalanche Warning", 35, "#1E90FF"},
    OfficialAlertStyle{"Earthquake Warning", 36, "#8B4513"},
    OfficialAlertStyle{"Volcano Warning", 37, "#2F4F4F"},
    OfficialAlertStyle{"Ashfall Warning", 38, "#A9A9A9"},
    OfficialAlertStyle{"Flood Warning", 39, "#00FF00"},
    OfficialAlertStyle{"Coastal Flood Warning", 40, "#228B22"},
    OfficialAlertStyle{"Lakeshore Flood Warning", 41, "#228B22"},
    OfficialAlertStyle{"Ashfall Advisory", 42, "#696969"},
    OfficialAlertStyle{"High Surf Warning", 43, "#228B22"},
    OfficialAlertStyle{"Extreme Heat Warning", 44, "#C71585"},
    OfficialAlertStyle{"Tornado Watch", 45, "#FFFF00"},
    OfficialAlertStyle{"Severe Thunderstorm Watch", 46, "#DB7093"},
    OfficialAlertStyle{"Flash Flood Watch", 47, "#2E8B57"},
    OfficialAlertStyle{"Gale Warning", 48, "#DDA0DD"},
    OfficialAlertStyle{"Flood Statement", 49, "#00FF00"},
    OfficialAlertStyle{"Extreme Cold Warning", 50, "#0000FF"},
    OfficialAlertStyle{"Freeze Warning", 51, "#483D8B"},
    OfficialAlertStyle{"Red Flag Warning", 52, "#FF1493"},
    OfficialAlertStyle{"Storm Surge Watch", 53, "#DB7FF7"},
    OfficialAlertStyle{"Hurricane Watch", 54, "#FF00FF"},
    OfficialAlertStyle{"Hurricane Force Wind Watch", 55, "#9932CC"},
    OfficialAlertStyle{"Typhoon Watch", 56, "#FF00FF"},
    OfficialAlertStyle{"Tropical Storm Watch", 57, "#F08080"},
    OfficialAlertStyle{"Storm Watch", 58, "#FFE4B5"},
    OfficialAlertStyle{"Tropical Cyclone Local Statement", 59, "#FFE4B5"},
    OfficialAlertStyle{"Winter Weather Advisory", 60, "#7B68EE"},
    OfficialAlertStyle{"Avalanche Advisory", 61, "#CD853F"},
    OfficialAlertStyle{"Cold Weather Advisory", 62, "#AFEEEE"},
    OfficialAlertStyle{"Heat Advisory", 63, "#FF7F50"},
    OfficialAlertStyle{"Flood Advisory", 64, "#00FF7F"},
    OfficialAlertStyle{"Coastal Flood Advisory", 65, "#7CFC00"},
    OfficialAlertStyle{"Lakeshore Flood Advisory", 66, "#7CFC00"},
    OfficialAlertStyle{"High Surf Advisory", 67, "#BA55D3"},
    OfficialAlertStyle{"Dense Fog Advisory", 68, "#708090"},
    OfficialAlertStyle{"Dense Smoke Advisory", 69, "#F0E68C"},
    OfficialAlertStyle{"Small Craft Advisory", 70, "#D8BFD8"},
    OfficialAlertStyle{"Brisk Wind Advisory", 71, "#D8BFD8"},
    OfficialAlertStyle{"Hazardous Seas Warning", 72, "#D8BFD8"},
    OfficialAlertStyle{"Dust Advisory", 73, "#BDB76B"},
    OfficialAlertStyle{"Blowing Dust Advisory", 74, "#BDB76B"},
    OfficialAlertStyle{"Lake Wind Advisory", 75, "#D2B48C"},
    OfficialAlertStyle{"Wind Advisory", 76, "#D2B48C"},
    OfficialAlertStyle{"Frost Advisory", 77, "#6495ED"},
    OfficialAlertStyle{"Freezing Fog Advisory", 78, "#008080"},
    OfficialAlertStyle{"Freezing Spray Advisory", 79, "#00BFFF"},
    OfficialAlertStyle{"Low Water Advisory", 80, "#A52A2A"},
    OfficialAlertStyle{"Local Area Emergency", 81, "#C0C0C0"},
    OfficialAlertStyle{"Winter Storm Watch", 82, "#4682B4"},
    OfficialAlertStyle{"Rip Current Statement", 83, "#40E0D0"},
    OfficialAlertStyle{"Beach Hazards Statement", 84, "#40E0D0"},
    OfficialAlertStyle{"Gale Watch", 85, "#FFC0CB"},
    OfficialAlertStyle{"Avalanche Watch", 86, "#F4A460"},
    OfficialAlertStyle{"Hazardous Seas Watch", 87, "#483D8B"},
    OfficialAlertStyle{"Heavy Freezing Spray Watch", 88, "#BC8F8F"},
    OfficialAlertStyle{"Flood Watch", 89, "#2E8B57"},
    OfficialAlertStyle{"Coastal Flood Watch", 90, "#66CDAA"},
    OfficialAlertStyle{"Lakeshore Flood Watch", 91, "#66CDAA"},
    OfficialAlertStyle{"High Wind Watch", 92, "#B8860B"},
    OfficialAlertStyle{"Extreme Heat Watch", 93, "#800000"},
    OfficialAlertStyle{"Extreme Cold Watch", 94, "#5F9EA0"},
    OfficialAlertStyle{"Freeze Watch", 95, "#00FFFF"},
    OfficialAlertStyle{"Fire Weather Watch", 96, "#FFDEAD"},
    OfficialAlertStyle{"Extreme Fire Danger", 97, "#E9967A"},
    OfficialAlertStyle{"911 Telephone Outage", 98, "#C0C0C0"},
    OfficialAlertStyle{"Coastal Flood Statement", 99, "#6B8E23"},
    OfficialAlertStyle{"Lakeshore Flood Statement", 100, "#6B8E23"},
    OfficialAlertStyle{"Special Weather Statement", 101, "#FFE4B5"},
    OfficialAlertStyle{"Marine Weather Statement", 102, "#FFDAB9"},
    OfficialAlertStyle{"Air Quality Alert", 103, "#808080"},
    OfficialAlertStyle{"Air Stagnation Advisory", 104, "#808080"},
    OfficialAlertStyle{"Hazardous Weather Outlook", 105, "#EEE8AA"},
    OfficialAlertStyle{"Hydrologic Outlook", 106, "#90EE90"},
    OfficialAlertStyle{"Short Term Forecast", 107, "#98FB98"},
    OfficialAlertStyle{"Administrative Message", 108, "#C0C0C0"},
    OfficialAlertStyle{"Test", 109, "#F0FFFF"},
    OfficialAlertStyle{"Child Abduction Emergency", 110, "#FFFFFF"},
    OfficialAlertStyle{"Blue Alert", 111, "#FFFFFF"},
};

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

NwsAlertStyle nwsAlertStyle(const QString& event) {
    const auto normalized = event.trimmed();
    for (const auto& style : kOfficialAlertStyles) {
        if (normalized.compare(QString::fromLatin1(style.event), Qt::CaseInsensitive) == 0)
            return {QColor(QString::fromLatin1(style.hex)), style.priority};
    }
    return {QColor(QStringLiteral("#F4D35E")), 1000};
}

NwsAlertStyle meteoalarmAlertStyle(const QString& severity) {
    // Meteoalarm awareness colors onto the NWS severity scale: red/orange for
    // Severe+, yellow for Moderate, grey for Minor. Priorities sit alongside
    // the NWS table so European warnings interleave sanely in the card list.
    const auto normalized = severity.trimmed();
    if (normalized.compare(QStringLiteral("Extreme"), Qt::CaseInsensitive) == 0)
        return {QColor(QStringLiteral("#FF0000")), 2};
    if (normalized.compare(QStringLiteral("Severe"), Qt::CaseInsensitive) == 0)
        return {QColor(QStringLiteral("#FF8C00")), 4};
    if (normalized.compare(QStringLiteral("Moderate"), Qt::CaseInsensitive) == 0)
        return {QColor(QStringLiteral("#FFD700")), 60};
    if (normalized.compare(QStringLiteral("Minor"), Qt::CaseInsensitive) == 0)
        return {QColor(QStringLiteral("#808080")), 100};
    return {QColor(QStringLiteral("#F4D35E")), 1000};
}

WeatherOverlaySource::WeatherOverlaySource(QObject* parent)
    : QObject(parent) {
    connect(&network_, &QNetworkAccessManager::finished, this, &WeatherOverlaySource::requestFinished);
    publishTimer_ = new QTimer(this);
    publishTimer_->setSingleShot(true);
    publishTimer_->setInterval(100);
    connect(publishTimer_, &QTimer::timeout, this, &WeatherOverlaySource::publishOverlaySnapshot);
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
        warning.id = feature.value(QStringLiteral("id")).toString();
        warning.event = properties.value(QStringLiteral("event")).toString();
        warning.headline = properties.value(QStringLiteral("headline")).toString();
        warning.severity = properties.value(QStringLiteral("severity")).toString();
        warning.senderName = properties.value(QStringLiteral("senderName")).toString();
        warning.description = properties.value(QStringLiteral("description")).toString();
        warning.instruction = properties.value(QStringLiteral("instruction")).toString();
        warning.onset = QDateTime::fromString(properties.value(QStringLiteral("onset")).toString(), Qt::ISODate);
        warning.expires = QDateTime::fromString(properties.value(QStringLiteral("expires")).toString(), Qt::ISODate);
        const auto alertStyle = nwsAlertStyle(warning.event);
        warning.alertColor = alertStyle.color;
        warning.alertPriority = alertStyle.priority;
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

QVector<WeatherWarning> WeatherOverlaySource::parseMeteoalarmAtom(const QByteArray& data,
                                                                  const QString& countrySlug,
                                                                  QString* error) {
    QVector<WeatherWarning> warnings;
    QXmlStreamReader xml(data);
    // Empty feeds (no active warnings) are a valid ~1 KB Atom shell.
    QString entryIdentifier;
    QString entryEvent;
    QString entrySeverity;
    QString entryCertainty;
    QString entryUrgency;
    QString entryOnset;
    QString entryExpires;
    QString entryEffective;
    QString entryAreaDesc;
    QString entryCapLink;
    QStringList entryGeocodes;
    bool inEntry = false;
    bool inGeocode = false;
    QString geocodeValueName;
    QString currentText;
    const auto flushEntry = [&] {
        if (entryIdentifier.isEmpty() && entryEvent.isEmpty()) return;
        WeatherWarning warning;
        warning.id = entryIdentifier.isEmpty()
                         ? QStringLiteral("meteoalarm:%1:%2").arg(countrySlug, entryCapLink)
                         : QStringLiteral("meteoalarm:%1").arg(entryIdentifier);
        warning.event = entryEvent.isEmpty() ? QStringLiteral("Weather warning") : entryEvent;
        warning.headline = entryAreaDesc.isEmpty()
                               ? warning.event
                               : QStringLiteral("%1 — %2").arg(entryAreaDesc, warning.event);
        warning.severity = entrySeverity.isEmpty() ? QStringLiteral("Unknown") : entrySeverity;
        warning.senderName = QStringLiteral("Meteoalarm/%1").arg(countrySlug);
        QStringList descriptionParts;
        if (!entryAreaDesc.isEmpty()) descriptionParts.push_back(entryAreaDesc);
        if (!entryGeocodes.isEmpty())
            descriptionParts.push_back(
                QStringLiteral("Areas: %1").arg(entryGeocodes.join(QStringLiteral(", "))));
        QStringList qualifierParts;
        if (!entryCertainty.isEmpty())
            qualifierParts.push_back(QStringLiteral("certainty %1").arg(entryCertainty));
        if (!entryUrgency.isEmpty())
            qualifierParts.push_back(QStringLiteral("urgency %1").arg(entryUrgency));
        if (!qualifierParts.isEmpty())
            descriptionParts.push_back(qualifierParts.join(QStringLiteral(", ")));
        if (!entryCapLink.isEmpty()) descriptionParts.push_back(entryCapLink);
        warning.description = descriptionParts.join(QStringLiteral(" · "));
        if (!entryOnset.isEmpty())
            warning.onset = QDateTime::fromString(entryOnset, Qt::ISODate);
        else if (!entryEffective.isEmpty())
            warning.onset = QDateTime::fromString(entryEffective, Qt::ISODate);
        if (!entryExpires.isEmpty())
            warning.expires = QDateTime::fromString(entryExpires, Qt::ISODate);
        const auto style = meteoalarmAlertStyle(warning.severity);
        warning.alertColor = style.color;
        warning.alertPriority = style.priority;
        // No polygons: Meteoalarm CAP carries EMMA/NUTS geocodes without
        // geometry. AlertCenter treats these as zone-only warnings (they
        // notify when the geographic filter is off and are skipped by the
        // proximity filter, same as unresolved NWS zones).
        warnings.push_back(std::move(warning));
    };
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const auto local = xml.name().toString();
            currentText.clear();
            if (local == QStringLiteral("entry")) {
                inEntry = true;
                entryIdentifier.clear();
                entryEvent.clear();
                entrySeverity.clear();
                entryCertainty.clear();
                entryUrgency.clear();
                entryOnset.clear();
                entryExpires.clear();
                entryEffective.clear();
                entryAreaDesc.clear();
                entryCapLink.clear();
                entryGeocodes.clear();
            } else if (inEntry && local == QStringLiteral("link")) {
                const auto attrs = xml.attributes();
                // The CAP detail link carries type="application/cap+xml".
                if (attrs.value(QStringLiteral("type")).toString() ==
                        QStringLiteral("application/cap+xml") &&
                    entryCapLink.isEmpty())
                    entryCapLink = attrs.value(QStringLiteral("href")).toString();
            } else if (inEntry && local == QStringLiteral("geocode")) {
                inGeocode = true;
                geocodeValueName.clear();
            }
        } else if (token == QXmlStreamReader::Characters) {
            if (inEntry && !xml.isWhitespace()) currentText += xml.text().toString();
        } else if (token == QXmlStreamReader::EndElement) {
            const auto local = xml.name().toString();
            const auto text = currentText.trimmed();
            currentText.clear();
            if (!inEntry) continue;
            if (local == QStringLiteral("entry")) {
                flushEntry();
                inEntry = false;
            } else if (local == QStringLiteral("identifier")) {
                if (entryIdentifier.isEmpty()) entryIdentifier = text;
            } else if (local == QStringLiteral("event")) {
                if (entryEvent.isEmpty()) entryEvent = text;
            } else if (local == QStringLiteral("severity")) {
                if (entrySeverity.isEmpty()) entrySeverity = text;
            } else if (local == QStringLiteral("certainty")) {
                if (entryCertainty.isEmpty()) entryCertainty = text;
            } else if (local == QStringLiteral("urgency")) {
                if (entryUrgency.isEmpty()) entryUrgency = text;
            } else if (local == QStringLiteral("onset")) {
                if (entryOnset.isEmpty()) entryOnset = text;
            } else if (local == QStringLiteral("expires")) {
                if (entryExpires.isEmpty()) entryExpires = text;
            } else if (local == QStringLiteral("effective")) {
                if (entryEffective.isEmpty()) entryEffective = text;
            } else if (local == QStringLiteral("areaDesc")) {
                if (entryAreaDesc.isEmpty()) entryAreaDesc = text;
            } else if (local == QStringLiteral("valueName")) {
                if (inGeocode) geocodeValueName = text;
            } else if (local == QStringLiteral("value")) {
                if (inGeocode && !text.isEmpty()) {
                    entryGeocodes.push_back(geocodeValueName.isEmpty()
                                                ? text
                                                : QStringLiteral("%1:%2").arg(geocodeValueName, text));
                }
            } else if (local == QStringLiteral("geocode")) {
                inGeocode = false;
                geocodeValueName.clear();
            }
        }
    }
    if (xml.hasError()) {
        if (error) *error = QStringLiteral("Invalid Meteoalarm feed %1: %2").arg(
                                countrySlug, xml.errorString());
        return {};
    }
    if (error) error->clear();
    return warnings;
}

void WeatherOverlaySource::refresh() {
    if (pending_ != 0) return;
    failures_ = 0;
    firstError_.clear();
    alertResponseReady_ = false;
    if (publishTimer_) publishTimer_->stop();
    // Report and Meteoalarm sets are intentionally retained: each successful
    // response replaces its own set wholesale, so a failed request keeps the
    // previously published contents instead of wiping overlays off the map
    // until the next success.
    pending_ = totalRequests_ = 7 + static_cast<int>(kMeteoalarmFeeds.size());
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
    for (const auto* slug : kMeteoalarmFeeds) {
        RequestContext context;
        context.kind = RequestKind::Meteoalarm;
        context.meteoalarmSlug = QString::fromLatin1(slug);
        // No Accept header: feeds.meteoalarm.org answers 406 to
        // "application/atom+xml" (verified 2026-09-06) but serves the same
        // feed for */* / text/html / no header. request() omits the header
        // when accept is empty.
        request(QUrl(meteoalarmFeedUrl(slug)), std::move(context), {});
    }
    emit statusChanged(QStringLiteral("updating NWS warnings, SPC reports, and Meteoalarm"));
}

void WeatherOverlaySource::request(const QUrl& url, RequestContext context, const QByteArray& accept) {
    auto request = makeRequest(url, QString::fromUtf8(accept));
    if (context.kind != RequestKind::Zone) {
        if (const auto etag = responseEtags_.value(url); !etag.isEmpty())
            request.setRawHeader(QByteArrayLiteral("If-None-Match"), etag);
        if (const auto modified = responseLastModified_.value(url); !modified.isEmpty())
            request.setRawHeader(QByteArrayLiteral("If-Modified-Since"), modified);
    }
    auto* reply = network_.get(request);
    requests_.insert(reply, std::move(context));
}

void WeatherOverlaySource::requestFinished(QNetworkReply* reply) {
    const auto context = requests_.take(reply);
    const auto url = reply->request().url();
    const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray data;
    bool responseAvailable = false;
    if (statusCode == 304 && context.kind != RequestKind::Zone && responseBodies_.contains(url)) {
        data = responseBodies_.value(url);
        responseAvailable = true;
    } else if (reply->error() == QNetworkReply::NoError) {
        data = reply->readAll();
        responseAvailable = true;
        if (context.kind != RequestKind::Zone) {
            responseBodies_.insert(url, data);
            if (const auto etag = reply->rawHeader(QByteArrayLiteral("ETag")); !etag.isEmpty())
                responseEtags_.insert(url, etag);
            if (const auto modified = reply->rawHeader(QByteArrayLiteral("Last-Modified")); !modified.isEmpty())
                responseLastModified_.insert(url, modified);
        }
    }
    if (!responseAvailable) {
        ++failures_;
        if (firstError_.isEmpty()) {
            firstError_ = statusCode == 304
                              ? QStringLiteral("overlay cache revalidation returned no cached body")
                              : reply->errorString();
        }
    } else {
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
                alertResponseReady_ = true;
                schedulePublish();
            }
        } else if (context.kind == RequestKind::Reports) {
            auto parsed = parseStormReports(data, context.reportKind, &error, context.convectiveDate);
            if (error.isEmpty() && context.reportSet >= 0)
                reportSets_.at(static_cast<std::size_t>(context.reportSet)) = std::move(parsed);
        } else if (context.kind == RequestKind::Meteoalarm) {
            auto parsed = parseMeteoalarmAtom(data, context.meteoalarmSlug, &error);
            if (error.isEmpty()) meteoalarmSets_.insert(context.meteoalarmSlug, std::move(parsed));
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
        if (alertResponseReady_) schedulePublish();
    }
    reply->deleteLater();
    --pending_;
    publishIfComplete();
}

void WeatherOverlaySource::publishIfComplete() {
    if (pending_ != 0) return;
    if (alertResponseReady_) {
        schedulePublish();
        return;
    }
    // The alerts request (or its parse) failed; still publish the stale-but-consistent
    // warnings and whatever report sets succeeded so they are not silently discarded.
    publishOverlaySnapshot();
}

void WeatherOverlaySource::schedulePublish() {
    if (!alertResponseReady_ || !publishTimer_ || publishTimer_->isActive()) return;
    publishTimer_->start();
}

void WeatherOverlaySource::publishOverlaySnapshot() {
    reports_.clear();
    const auto cutoff = QDateTime::currentDateTimeUtc().addSecs(-24 * 60 * 60);
    for (const auto& set : reportSets_) {
        for (const auto& report : set) {
            if (!report.occurredAt.isValid() || report.occurredAt >= cutoff) reports_.push_back(report);
        }
    }
    QVector<WeatherWarning> combined = warnings_;
    qsizetype meteoalarmCount = 0;
    // Deterministic feed order so the merged list is stable across refreshes.
    auto slugs = meteoalarmSets_.keys();
    std::sort(slugs.begin(), slugs.end());
    for (const auto& slug : slugs) {
        const auto& set = meteoalarmSets_.value(slug);
        meteoalarmCount += set.size();
        for (const auto& warning : set) combined.push_back(warning);
    }
    emit overlaysReady(combined, reports_, QDateTime::currentDateTimeUtc());
    if (pending_ != 0) return;
    if (failures_ == 0) {
        const auto drawable = std::count_if(combined.cbegin(), combined.cend(),
                                            [](const auto& warning) { return !warning.polygons.isEmpty(); });
        emit statusChanged(QStringLiteral("%1 drawable warnings (%2 NWS + %3 Meteoalarm) | %4 reports (24 h)")
                               .arg(drawable).arg(warnings_.size()).arg(meteoalarmCount).arg(reports_.size()));
    } else {
        emit statusChanged(QStringLiteral("overlay update partial (%1 of %2 requests failed): %3")
                               .arg(failures_).arg(totalRequests_).arg(firstError_));
    }
}

} // namespace cappedweather
