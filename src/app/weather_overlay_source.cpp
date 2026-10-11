#include "app/weather_overlay_source.hpp"
#include "app/http_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#ifdef CAPPEDWEATHER_HAS_ZLIB
#include <zlib.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>

namespace cappedweather {

namespace {
constexpr auto kAlertsUrl = "https://api.weather.gov/alerts/active?status=actual&message_type=alert";
// MSC GeoMet weather-alerts collection (OGC API Features, open, no key).
// Canada-wide bbox in OGC lon/lat order; the collection only holds active
// products so no time filter is needed.
constexpr auto kEcccAlertsUrl =
    "https://api.weather.gc.ca/collections/weather-alerts/items?f=json&lang=en&limit=400&bbox=-141,42,-52,72";
constexpr auto kTornadoReportsUrl = "https://www.spc.noaa.gov/climo/reports/today_torn.csv";
constexpr auto kHailReportsUrl = "https://www.spc.noaa.gov/climo/reports/today_hail.csv";
constexpr auto kWindReportsUrl = "https://www.spc.noaa.gov/climo/reports/today_wind.csv";
constexpr auto kYesterdayTornadoReportsUrl = "https://www.spc.noaa.gov/climo/reports/yesterday_torn.csv";
constexpr auto kYesterdayHailReportsUrl = "https://www.spc.noaa.gov/climo/reports/yesterday_hail.csv";
constexpr auto kYesterdayWindReportsUrl = "https://www.spc.noaa.gov/climo/reports/yesterday_wind.csv";

// Meteoalarm visualization service (EUMETNET, same backend as
// meteoalarm.org): official warning-area polygons + warning texts.
// visservice region codes are ISO-2 and cover the OPERA radar footprint.
constexpr char kMeteoVisBase[] = "https://visservice.meteoalarm.org/api/v1";
constexpr char kMeteoTileBase[] = "https://visservice.meteoalarm.org";
constexpr std::array kMeteoRegions{
    "FR", "DE", "NL", "BE", "FI", "SE", "NO", "DK",
    "IE", "UK", "ES", "PL", "IT", "AT",
};
constexpr int kMeteoTileZoom = 5;
constexpr int kMeteoMaxNewTiles = 24;
constexpr int kMeteoMaxNewDetails = 40;

QString meteoReferenceDateString(const QDateTime& referenceDate) {
    // Hour-truncated UTC ISO with Z suffix (the only form verified against
    // the service); stable for an hour so tile URLs stay cacheable.
    return referenceDate.toString(QStringLiteral("yyyy-MM-ddTHH:00:00'Z'"));
}

QDateTime meteoInstant(const QString& text) {
    // "2026-09-06T14:02:19.000000Z" -> Qt-parseable ISO (fractional seconds
    // stripped, Z normalized to an explicit offset).
    QString normalized = text.trimmed();
    const auto dot = normalized.indexOf(QLatin1Char('.'));
    if (dot >= 0) {
        int end = dot;
        while (end < normalized.size() && (normalized.at(end) == QLatin1Char('.') ||
                                           normalized.at(end).isDigit()))
            ++end;
        normalized.remove(dot, end - dot);
    }
    if (normalized.endsWith(QLatin1Char('Z'))) {
        normalized.chop(1);
        normalized += QStringLiteral("+00:00");
    }
    return QDateTime::fromString(normalized, Qt::ISODate);
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
    } else if (type == QStringLiteral("GeometryCollection")) {
        // NWS forecast zones (e.g. coastal FLZ counties) group mainland
        // plus island MultiPolygons: recurse into members with the same
        // polygon handling. Anything else stays ignored and fail-closed.
        for (const auto& memberValue :
             geometry.value(QStringLiteral("geometries")).toArray()) {
            if (memberValue.isObject()) appendGeometry(memberValue.toObject(), polygons);
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

bool meteoTilesAvailable() {
#ifdef CAPPEDWEATHER_HAS_ZLIB
    return true;
#else
    return false;
#endif
}

QDateTime WeatherOverlaySource::meteoReferenceDate(const QDateTime& now) {
    const auto utc = now.toUTC();
    return QDateTime(utc.date(), QTime(utc.time().hour(), 0, 0), QTimeZone::UTC);
}

NwsAlertStyle WeatherOverlaySource::meteoalarmLevelStyle(const int levelId) {
    // Official Meteoalarm awareness colors (visservice levels endpoint).
    switch (levelId) {
    case 1: return {QColor(QStringLiteral("#FF0000")), 2};   // Extreme / red
    case 2: return {QColor(QStringLiteral("#FF9300")), 4};   // Severe / orange
    case 3: return {QColor(QStringLiteral("#FFDA22")), 60};  // Moderate / yellow
    default: break;
    }
    return {QColor(QStringLiteral("#F4D35E")), 1000};
}

QString WeatherOverlaySource::meteoSeverityForLevel(const int levelId) {
    switch (levelId) {
    case 1: return QStringLiteral("Extreme");
    case 2: return QStringLiteral("Severe");
    case 3: return QStringLiteral("Moderate");
    default: break;
    }
    return QStringLiteral("Unknown");
}

QUrl WeatherOverlaySource::meteoTileUrl(const int z, const int x, const int y,
                                        const QDateTime& referenceDate) {
    QUrl url(QStringLiteral("%1/stream-buffers/live/tiles/%2/%3/%4.mvt")
                 .arg(QString::fromLatin1(kMeteoTileBase))
                 .arg(z)
                 .arg(x)
                 .arg(y));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("referenceDate"), meteoReferenceDateString(referenceDate));
    query.addQueryItem(QStringLiteral("preset"), QStringLiteral("now"));
    url.setQuery(query);
    return url;
}

QVector<QPair<int, int>> WeatherOverlaySource::meteoTilesForBounds(const double minLon,
                                                                   const double minLat,
                                                                   const double maxLon,
                                                                   const double maxLat) {
    // Covering z5 tiles for a bbox (Web Mercator XYZ), clamped to Europe.
    // z5 keeps whole warned regions to a handful of requests.
    const auto lonToX = [](const double lon) {
        return static_cast<int>(std::floor((lon + 180.0) / 360.0 * 32.0));
    };
    const auto latToY = [](const double lat) {
        const double clamped = std::clamp(lat, -85.0, 85.0);
        const double sin = std::sin(clamped * 3.14159265358979323846 / 180.0);
        return static_cast<int>(
            std::floor((0.5 - std::log((1.0 + sin) / (1.0 - sin)) / (4.0 * 3.14159265358979323846)) *
                       32.0));
    };
    const int x0 = std::clamp(lonToX(minLon), 0, 31);
    const int x1 = std::clamp(lonToX(maxLon), 0, 31);
    const int y0 = std::clamp(latToY(maxLat), 0, 31);
    const int y1 = std::clamp(latToY(minLat), 0, 31);
    QVector<QPair<int, int>> tiles;
    for (int x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) tiles.push_back({x, y});
    }
    return tiles;
}

QString WeatherOverlaySource::localizedText(const QJsonValue& value) {
    if (value.isString()) return value.toString().trimmed();
    if (!value.isObject()) return {};
    const auto map = value.toObject();
    const auto english = map.value(QStringLiteral("en")).toString().trimmed();
    if (!english.isEmpty()) return english;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        const auto text = it.value().toString().trimmed();
        if (!text.isEmpty()) return text;
    }
    return {};
}

WeatherOverlaySource::WeatherOverlaySource(QObject* parent)
    : QObject(parent) {    connect(&network_, &QNetworkAccessManager::finished, this, &WeatherOverlaySource::requestFinished);
    publishTimer_ = new QTimer(this);
    publishTimer_->setSingleShot(true);
    publishTimer_->setInterval(100);
    connect(publishTimer_, &QTimer::timeout, this, &WeatherOverlaySource::publishOverlaySnapshot);
    // Warm-start fast path: static zone boundaries preload from disk, so a
    // restart with active alerts skips refetching unchanged geometry.
    loadZoneDiskCache();
}

namespace {

// Zone disk cache bounds: reuse bodies under 7 days old without any
// request, prune entries older than 30 days, cap the file at 3000 zones
// (CONUS has ~3k county zones; fire-weather zones ride the same file)
// and 64 MB of bodies (coastal zones run near 1 MB each, so the entry
// cap alone cannot bound disk use).
constexpr qint64 kZoneReuseSecs = 7 * 86400;
constexpr qint64 kZonePruneSecs = 30 * 86400;
constexpr int kZoneDiskCap = 3000;
constexpr qint64 kZoneDiskBytes = 64 * 1024 * 1024;
constexpr int kZoneDiskVersion = 1;

}  // namespace

QByteArray WeatherOverlaySource::serializeZoneDiskCache(const ZoneDiskCache& cache, QString* error) {
    if (error) error->clear();
    QJsonObject zones;
    for (auto it = cache.cbegin(); it != cache.cend(); ++it) {
        const auto& entry = it.value();
        if (it.key().isEmpty() || entry.body.isEmpty() || entry.fetchedAtSecs <= 0) {
            if (error) *error = QStringLiteral("zone cache entry is incomplete");
            return {};
        }
        QJsonObject record;
        record.insert(QStringLiteral("body"), QString::fromLatin1(entry.body.toBase64()));
        record.insert(QStringLiteral("etag"), QString::fromUtf8(entry.etag));
        record.insert(QStringLiteral("modified"), QString::fromUtf8(entry.modified));
        record.insert(QStringLiteral("fetchedAt"), entry.fetchedAtSecs);
        zones.insert(it.key(), record);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kZoneDiskVersion);
    root.insert(QStringLiteral("zones"), zones);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

WeatherOverlaySource::ZoneDiskCache WeatherOverlaySource::parseZoneDiskCache(const QByteArray& data) {
    ZoneDiskCache cache;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return cache;
    const auto root = document.object();
    if (root.value(QStringLiteral("version")).toInt(-1) != kZoneDiskVersion) return cache;
    const auto zones = root.value(QStringLiteral("zones"));
    if (!zones.isObject()) return cache;
    const auto objects = zones.toObject();
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        if (!it.value().isObject()) return ZoneDiskCache{};
        const auto record = it.value().toObject();
        const auto body = QByteArray::fromBase64(record.value(QStringLiteral("body")).toString().toLatin1());
        const auto fetchedAt = record.value(QStringLiteral("fetchedAt")).toVariant().toLongLong();
        if (it.key().isEmpty() || body.isEmpty() || fetchedAt <= 0) return ZoneDiskCache{};
        ZoneDiskEntry entry;
        entry.body = body;
        entry.etag = record.value(QStringLiteral("etag")).toString().toUtf8();
        entry.modified = record.value(QStringLiteral("modified")).toString().toUtf8();
        entry.fetchedAtSecs = fetchedAt;
        cache.insert(it.key(), entry);
    }
    return cache;
}

void WeatherOverlaySource::setZoneDiskCachePath(const QString& path) {
    zoneDiskCacheOverride_ = path;
    loadZoneDiskCache();
}

QStringList WeatherOverlaySource::selectZoneDiskUrls(const ZoneDiskCache& cache, const qint64 maxBytes) {
    auto urls = cache.keys();
    std::sort(urls.begin(), urls.end(), [&](const QString& a, const QString& b) {
        return cache.value(a).fetchedAtSecs > cache.value(b).fetchedAtSecs;
    });
    QStringList kept;
    qint64 bytes = 0;
    for (const auto& url : urls) {
        const qint64 size = cache.value(url).body.size();
        if (!kept.isEmpty() && bytes + size > maxBytes) break;
        kept.push_back(url);
        bytes += size;
    }
    return kept;
}

QString WeatherOverlaySource::zoneDiskCachePath() const {
    if (!zoneDiskCacheOverride_.isEmpty()) return zoneDiskCacheOverride_;
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (dir.isEmpty()) return {};
    return dir + QStringLiteral("/overlay_zone_cache.json");
}

bool WeatherOverlaySource::zoneGeometryFresh(const QString& zoneUrl, const qint64 nowSecs) const {
    const auto fetchedAt = zoneFetchedAt_.value(zoneUrl, 0);
    return fetchedAt > 0 && nowSecs >= fetchedAt && nowSecs - fetchedAt <= kZoneReuseSecs;
}

void WeatherOverlaySource::loadZoneDiskCache() {
    const auto path = zoneDiskCachePath();
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto cache = parseZoneDiskCache(file.readAll());
    file.close();
    if (cache.isEmpty()) return;
    // Replace semantics: the file is the whole warmed set, so a re-pointed
    // source (and every fresh construction) matches it exactly instead of
    // accumulating live arrivals across loads.
    zoneCache_.clear();
    zoneFetchedAt_.clear();
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    // Prune before budgeting so ancient entries cannot crowd fresh ones
    // out of the byte allowance; survivors are newest-first under both
    // caps (see saveZoneDiskCache).
    ZoneDiskCache fresh;
    for (auto it = cache.cbegin(); it != cache.cend(); ++it) {
        if (nowSecs - it.value().fetchedAtSecs <= kZonePruneSecs) fresh.insert(it.key(), it.value());
    }
    int kept = 0;
    for (const auto& url : selectZoneDiskUrls(fresh, kZoneDiskBytes)) {
        const auto& entry = fresh[url];
        if (kept >= kZoneDiskCap) break;
        QString error;
        const auto polygons = parseZone(entry.body, &error);
        if (!error.isEmpty() || polygons.isEmpty()) continue;
        zoneCache_.insert(url, polygons);
        zoneFetchedAt_.insert(url, entry.fetchedAtSecs);
        const QUrl key(url);
        responseBodies_.insert(key, entry.body);
        if (!entry.etag.isEmpty()) responseEtags_.insert(key, entry.etag);
        if (!entry.modified.isEmpty()) responseLastModified_.insert(key, entry.modified);
        ++kept;
    }
}

void WeatherOverlaySource::saveZoneDiskCache() {
    const auto path = zoneDiskCachePath();
    if (path.isEmpty()) return;
    ZoneDiskCache cache;
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    for (auto it = zoneCache_.cbegin(); it != zoneCache_.cend(); ++it) {
        const QUrl key(it.key());
        const auto body = responseBodies_.value(key);
        if (body.isEmpty()) continue;
        ZoneDiskEntry entry;
        entry.body = body;
        entry.etag = responseEtags_.value(key);
        entry.modified = responseLastModified_.value(key);
        entry.fetchedAtSecs = zoneFetchedAt_.value(it.key(), nowSecs);
        cache.insert(it.key(), entry);
    }
    if (cache.isEmpty()) return;
    // Newest-first under both caps so one stormy week cannot grow the file
    // without bound; dropped entries simply refetch next launch.
    ZoneDiskCache budgeted;
    int kept = 0;
    for (const auto& url : selectZoneDiskUrls(cache, kZoneDiskBytes)) {
        if (kept >= kZoneDiskCap) break;
        budgeted.insert(url, cache.value(url));
        ++kept;
    }
    if (budgeted.isEmpty()) return;
    QString error;
    const auto data = serializeZoneDiskCache(budgeted, &error);
    if (!error.isEmpty() || data.isEmpty()) return;
    QFile file(path);
    const auto dir = QFileInfo(path).dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) return;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(data);
}

QString warningCountdownText(const QDateTime& expires, const QDateTime& now) {
    if (!expires.isValid() || !now.isValid()) return QStringLiteral("no expiry listed");
    const qint64 secs = now.secsTo(expires);
    if (secs <= 0) return QStringLiteral("expired");
    if (secs < 60) return QStringLiteral("<1m left");
    if (secs < 3600) return QStringLiteral("%1m left").arg(secs / 60);
    if (secs < 86400) return QStringLiteral("%1h %2m left").arg(secs / 3600).arg((secs % 3600) / 60);
    return QStringLiteral("%1d %2h left").arg(secs / 86400).arg((secs % 86400) / 3600);
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

QVector<WeatherWarning> WeatherOverlaySource::parseEcccAlerts(const QByteArray& data,
                                                             QString* error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid ECCC alert GeoJSON: %1").arg(parseError.errorString());
        return {};
    }
    const auto titleCase = [](QString raw) {
        raw = raw.toLower();
        bool wordStart = true;
        for (auto& ch : raw) {
            if (ch.isSpace()) {
                wordStart = true;
            } else if (wordStart) {
                ch = ch.toUpper();
                wordStart = false;
            }
        }
        return raw;
    };
    QVector<WeatherWarning> warnings;
    for (const auto& value : document.object().value(QStringLiteral("features")).toArray()) {
        const auto feature = value.toObject();
        const auto geometryValue = feature.value(QStringLiteral("geometry"));
        const auto properties = feature.value(QStringLiteral("properties")).toObject();
        WeatherWarning warning;
        // Feature ids embed the issuance batch, so reissues mint new ids;
        // the eccc: prefix keeps AlertCenter dedupe in the ECCC namespace.
        warning.id = QStringLiteral("eccc:%1").arg(feature.value(QStringLiteral("id")).toString());
        // Title-cased on purpose: names shared with NWS ("Special Weather
        // Statement", "Blizzard Warning") inherit official colors through
        // the case-insensitive style table; the rest take the neutral
        // fallback instead of a borrowed severity color.
        warning.event = titleCase(properties.value(QStringLiteral("alert_name_en")).toString());
        if (warning.event.isEmpty()) continue;
        const auto area = properties.value(QStringLiteral("feature_name_en")).toString();
        warning.headline = area.isEmpty() ? warning.event
                                          : QStringLiteral("%1: %2").arg(area, warning.event);
        // ECCC alert_type vocabulary is small and stable; anything else
        // stays Unknown (AlertCenter treats Unknown as Moderate).
        const auto type = properties.value(QStringLiteral("alert_type")).toString().toLower();
        warning.severity = type == QStringLiteral("warning")   ? QStringLiteral("Severe")
                           : type == QStringLiteral("watch")  ? QStringLiteral("Moderate")
                           : type == QStringLiteral("advisory") ||
                                   type == QStringLiteral("statement")
                                                               ? QStringLiteral("Minor")
                                                               : QStringLiteral("Unknown");
        warning.senderName = QStringLiteral("Environment Canada");
        warning.description = properties.value(QStringLiteral("alert_text_en")).toString();
        warning.onset =
            QDateTime::fromString(properties.value(QStringLiteral("publication_datetime")).toString(),
                                  Qt::ISODate);
        warning.expires =
            QDateTime::fromString(properties.value(QStringLiteral("expiration_datetime")).toString(),
                                  Qt::ISODate);
        // No status_en filtering: ended/cancelled values appear on otherwise
        // current products, so unexpired items publish and expiry alone
        // retires them, mirroring the NWS path.
        const auto alertStyle = nwsAlertStyle(warning.event);
        warning.alertColor = alertStyle.color;
        warning.alertPriority = alertStyle.priority;
        if (geometryValue.isObject()) appendGeometry(geometryValue.toObject(), warning.polygons);
        if (!warning.polygons.isEmpty()) warnings.push_back(std::move(warning));
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

namespace {

// Minimal protobuf reader for Mapbox Vector Tiles: varints, length-delimited
// blobs, and fixed32/64 skips. Anything malformed fails closed via ok().
struct MvtCursor {
    const char* ptr = nullptr;
    const char* end = nullptr;
    bool ok = true;

    bool atEnd() const { return !ok || ptr >= end; }

    quint64 varint() {
        quint64 result = 0;
        int shift = 0;
        while (ptr < end && shift < 64) {
            const auto byte = static_cast<unsigned char>(*ptr++);
            result |= static_cast<quint64>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return result;
            shift += 7;
        }
        ok = false;
        return 0;
    }

    // Splits a tag varint into field number and wire type.
    bool tag(quint64* number, quint64* wire) {
        if (atEnd()) return false;
        const quint64 tag = varint();
        if (!ok) return false;
        *number = tag >> 3;
        *wire = tag & 7;
        return true;
    }

    void skip(const quint64 wire) {
        if (!ok) return;
        if (wire == 0) {
            varint();
        } else if (wire == 1) {
            if (end - ptr < 8) ok = false;
            else ptr += 8;
        } else if (wire == 2) {
            const quint64 length = varint();
            if (!ok || static_cast<quint64>(end - ptr) < length) ok = false;
            else ptr += length;
        } else if (wire == 5) {
            if (end - ptr < 4) ok = false;
            else ptr += 4;
        } else {
            ok = false;  // groups (3/4) never appear in MVT; anything else is corrupt
        }
    }

    QByteArray bytes() {
        const quint64 length = varint();
        if (!ok || length > static_cast<quint64>(end - ptr)) {
            ok = false;
            return {};
        }
        const QByteArray result(ptr, static_cast<qsizetype>(length));
        ptr += length;
        return result;
    }

    QString string() { return QString::fromUtf8(bytes()); }
};

qint32 mvtZigzag(const quint64 value) {
    return static_cast<qint32>((value >> 1) ^ (~(value & 1) + 1));
}

// One decoded MVT ring in tile coordinates plus its shoelace area.
struct MvtRing {
    QPolygonF points;
    double area = 0.0;
};

QVector<MvtRing> mvtDecodeRings(const QByteArray& geometry, const int maxRings = 64,
                                const int maxPoints = 1500) {
    QVector<MvtRing> rings;
    MvtCursor cursor{geometry.constData(), geometry.constData() + geometry.size()};
    long long cursorX = 0;
    long long cursorY = 0;
    MvtRing current;
    bool open = false;
    const auto pushRing = [&] {
        // Triangles carry 3 points with implicit closure; close explicitly.
        if (open && current.points.size() >= 3) {
            // Close explicitly and measure (tile space is y-down).
            if (current.points.first() != current.points.last())
                current.points.push_back(current.points.first());
            double area = 0.0;
            const auto& points = current.points;
            for (qsizetype i = 0; i < points.size(); ++i) {
                const auto& a = points.at(i);
                const auto& b = points.at((i + 1) % points.size());
                area += a.x() * b.y() - b.x() * a.y();
            }
            current.area = area / 2.0;
            rings.push_back(std::move(current));
        }
        current = MvtRing{};
        open = false;
    };
    while (!cursor.atEnd() && rings.size() < maxRings) {
        const quint64 command = cursor.varint();
        if (!cursor.ok) return {};
        const quint64 id = command & 7;
        quint64 count = command >> 3;
        if (id == 1 || id == 2) {
            while (count-- > 0) {
                if (cursor.atEnd()) return {};
                const qint32 dx = mvtZigzag(cursor.varint());
                if (cursor.atEnd()) return {};
                const qint32 dy = mvtZigzag(cursor.varint());
                if (!cursor.ok) return {};
                cursorX += dx;
                cursorY += dy;
                if (id == 1) {
                    pushRing();
                    current.points.push_back(
                        QPointF(static_cast<double>(cursorX), static_cast<double>(cursorY)));
                    open = true;
                } else if (open) {
                    current.points.push_back(
                        QPointF(static_cast<double>(cursorX), static_cast<double>(cursorY)));
                }
                if (open && current.points.size() > maxPoints * 2) {
                    // Pathological ring: stride-sample to stay bounded.
                    QPolygonF sampled;
                    sampled.reserve(maxPoints + 1);
                    const qsizetype step =
                        std::max<qsizetype>(1, current.points.size() / maxPoints);
                    for (qsizetype i = 0; i < current.points.size(); i += step)
                        sampled.push_back(current.points.at(i));
                    current.points = std::move(sampled);
                }
            }
        } else if (id == 7) {
            // ClosePath: count is always 1; extra closes are harmless.
            for (quint64 i = 0; i < count; ++i) pushRing();
        } else {
            return {};
        }
    }
    pushRing();
    return cursor.ok ? rings : QVector<MvtRing>{};
}

#ifdef CAPPEDWEATHER_HAS_ZLIB
QByteArray gunzipBytes(const QByteArray& gzip, QString* error) {
    // RFC 1952 gzip: 10-byte header, raw deflate, 8-byte trailer.
    if (gzip.size() < 18 || static_cast<unsigned char>(gzip.at(0)) != 0x1F ||
        static_cast<unsigned char>(gzip.at(1)) != 0x8B) {
        if (error) *error = QStringLiteral("MVT tile is not gzip data");
        return {};
    }
    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        if (error) *error = QStringLiteral("MVT inflate init failed");
        return {};
    }
    QByteArray out;
    out.reserve(std::min<qsizetype>(gzip.size() * 3, 8 * 1024 * 1024));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(gzip.constData()));
    stream.avail_in = static_cast<uInt>(gzip.size());
    char buffer[65536];
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        status = inflate(&stream, Z_NO_FLUSH);
        if (status == Z_OK || status == Z_STREAM_END) {
            out.append(buffer, sizeof(buffer) - stream.avail_out);
        }
        if (out.size() > 8 * 1024 * 1024) {
            inflateEnd(&stream);
            if (error) *error = QStringLiteral("MVT tile exceeds size cap");
            return {};
        }
    }
    inflateEnd(&stream);
    if (status != Z_STREAM_END) {
        if (error) *error = QStringLiteral("MVT inflate failed");
        return {};
    }
    return out;
}
#endif

} // namespace

QHash<QString, QVector<WarningPolygon>> WeatherOverlaySource::parseMvtAreas(
    const QByteArray& gzipData, const int z, const int x, const int y,
    const QSet<QString>& wantedUuids, QString* error) {
    QHash<QString, QVector<WarningPolygon>> result;
    if (error) error->clear();
    if (wantedUuids.isEmpty()) return result;
#ifndef CAPPEDWEATHER_HAS_ZLIB
    Q_UNUSED(gzipData);
    Q_UNUSED(z);
    Q_UNUSED(x);
    Q_UNUSED(y);
    if (error) *error = QStringLiteral("MVT tiles unavailable: build without zlib");
    return result;
#else
    QString gunzipError;
    // Qt transparently decodes Content-Encoding: gzip, so depending on the
    // stack the payload arrives raw protobuf already; gunzip only when the
    // gzip magic is present.
    const bool gzipped = gzipData.size() >= 2 &&
                         static_cast<unsigned char>(gzipData.at(0)) == 0x1F &&
                         static_cast<unsigned char>(gzipData.at(1)) == 0x8B;
    const QByteArray pbf =
        gzipped ? gunzipBytes(gzipData, &gunzipError) : gzipData;
    if (pbf.isEmpty()) {
        if (error) *error = gzipped ? gunzipError : QStringLiteral("MVT tile is empty");
        return result;
    }
    const double tiles = static_cast<double>(1 << std::min(std::max(z, 0), 22));
    MvtCursor tile{pbf.constData(), pbf.constData() + pbf.size()};
    bool corrupt = false;
    while (!tile.atEnd()) {
        quint64 number = 0;
        quint64 wire = 0;
        if (!tile.tag(&number, &wire)) break;
        if (number != 3 || wire != 2) {
            tile.skip(wire);
            continue;
        }
        const QByteArray layerBytes = tile.bytes();
        if (!tile.ok) break;
        MvtCursor layer{layerBytes.constData(), layerBytes.constData() + layerBytes.size()};
        QString layerName;
        QStringList keys;
        QStringList values;
        QVector<QByteArray> features;
        quint64 extent = 4096;
        while (!layer.atEnd()) {
            quint64 field = 0;
            quint64 fieldWire = 0;
            if (!layer.tag(&field, &fieldWire)) break;
            if (field == 1 && fieldWire == 2) {
                layerName = layer.string();
            } else if (field == 2 && fieldWire == 2) {
                features.push_back(layer.bytes());
            } else if (field == 3 && fieldWire == 2) {
                keys.push_back(layer.string());
            } else if (field == 4 && fieldWire == 2) {
                const QByteArray valueBytes = layer.bytes();
                MvtCursor value{valueBytes.constData(), valueBytes.constData() + valueBytes.size()};
                QString text;
                while (!value.atEnd()) {
                    quint64 valueField = 0;
                    quint64 valueWire = 0;
                    if (!value.tag(&valueField, &valueWire)) break;
                    if (valueField == 1 && valueWire == 2) {
                        text = value.string();
                    } else if ((valueField == 2 && valueWire == 5) ||
                               (valueField == 3 && valueWire == 1)) {
                        // float/double: irrelevant for uuid matching
                        value.skip(valueWire);
                    } else if (valueField >= 4 && valueField <= 7 && valueWire == 0) {
                        text = QString::number(value.varint());
                    } else {
                        value.skip(valueWire);
                    }
                }
                values.push_back(text);
            } else if (field == 5 && fieldWire == 0) {
                extent = layer.varint();
            } else {
                layer.skip(fieldWire);
            }
            if (!layer.ok) break;
        }
        if (!layer.ok || layerName != QStringLiteral("areas")) continue;
        const int uuidKey = keys.indexOf(QStringLiteral("uuid"));
        if (uuidKey < 0) continue;
        const double scale = extent > 0 ? static_cast<double>(extent) : 4096.0;
        for (const auto& featureBytes : features) {
            MvtCursor feature{featureBytes.constData(),
                              featureBytes.constData() + featureBytes.size()};
            QVector<quint64> tags;
            QByteArray geometry;
            while (!feature.atEnd()) {
                quint64 field = 0;
                quint64 fieldWire = 0;
                if (!feature.tag(&field, &fieldWire)) break;
                if (field == 2 && fieldWire == 2) {
                    const QByteArray packed = feature.bytes();
                    MvtCursor packedCursor{packed.constData(), packed.constData() + packed.size()};
                    while (!packedCursor.atEnd()) tags.push_back(packedCursor.varint());
                } else if (field == 4 && fieldWire == 2) {
                    geometry = feature.bytes();
                } else {
                    feature.skip(fieldWire);
                }
                if (!feature.ok) break;
            }
            if (!feature.ok || geometry.isEmpty()) continue;
            QString uuid;
            for (qsizetype i = 0; i + 1 < tags.size(); i += 2) {
                if (tags.at(i) == static_cast<quint64>(uuidKey) &&
                    tags.at(i + 1) < static_cast<quint64>(values.size())) {
                    uuid = values.at(static_cast<qsizetype>(tags.at(i + 1)));
                    break;
                }
            }
            if (!wantedUuids.contains(uuid)) continue;
            const auto rings = mvtDecodeRings(geometry);
            QVector<WarningPolygon> polygons;
            WarningPolygon* current = nullptr;
            double currentArea = 0.0;
            for (const auto& ring : rings) {
                if (ring.area == 0.0) continue;  // degenerate
                // Tile space is y-down: positive shoelace = exterior.
                QPolygonF exterior;
                exterior.reserve(ring.points.size());
                for (const auto& tilePoint : ring.points) {
                    const double lon =
                        (static_cast<double>(x) + tilePoint.x() / scale) / tiles * 360.0 - 180.0;
                    const double mercator =
                        (static_cast<double>(y) + tilePoint.y() / scale) / tiles;
                    const double lat = std::atan(std::sinh(
                                           3.14159265358979323846 * (1.0 - 2.0 * mercator))) *
                                       180.0 / 3.14159265358979323846;
                    exterior.push_back(QPointF(std::clamp(lon, -180.0, 180.0),
                                               std::clamp(lat, -85.0, 85.0)));
                }
                if (exterior.size() < 4) continue;
                if (ring.area > 0.0) {
                    polygons.push_back(WarningPolygon{});
                    current = &polygons.last();
                    current->exterior = std::move(exterior);
                    currentArea = ring.area;
                } else if (current != nullptr && -ring.area < currentArea) {
                    current->holes.push_back(std::move(exterior));
                } else {
                    // Oversized or orphan hole: keep it as its own exterior.
                    polygons.push_back(WarningPolygon{});
                    current = &polygons.last();
                    current->exterior = std::move(exterior);
                    currentArea = -ring.area;
                }
            }
            if (!polygons.isEmpty()) result[uuid].append(polygons);
        }
        if (!layer.ok) corrupt = true;
    }
    if (!tile.ok) corrupt = true;
    if (corrupt && result.isEmpty() && error && error->isEmpty()) {
        *error = QStringLiteral("corrupt MVT tile");
    }
    return result;
#endif
}
void WeatherOverlaySource::refresh() {
    if (pending_ != 0) return;
    failures_ = 0;
    firstError_.clear();
    alertResponseReady_ = false;
    if (publishTimer_) publishTimer_->stop();
    // Report sets are intentionally retained: each successful response replaces its
    // own set wholesale, so a failed request keeps the previously published contents
    // instead of wiping storm reports off the map until the next success.
    // Meteoalarm follows the same pattern per country (areas/hazards), while
    // details and tile geometry accumulate in caches.
    pending_ = totalRequests_ = 8;
    zoneWarningIndexes_.clear();
    meteoPendingDetails_.clear();
    meteoPendingTiles_.clear();
    meteoReferenceDate_ = meteoReferenceDate(QDateTime::currentDateTimeUtc());
    pruneMeteoTileCache();
    const auto now = QDateTime::currentDateTimeUtc();
    const auto today = now.time().hour() < 12 ? now.date().addDays(-1) : now.date();
    request(QUrl(QString::fromUtf8(kAlertsUrl)), {RequestKind::Alerts}, QByteArrayLiteral("application/geo+json"));
    request(QUrl(QString::fromUtf8(kEcccAlertsUrl)), {RequestKind::EcccAlerts},
            QByteArrayLiteral("application/geo+json"));
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
    if (!meteoTypesRequested_) {
        meteoTypesRequested_ = true;
        ++pending_;
        ++totalRequests_;
        RequestContext typesContext;
        typesContext.kind = RequestKind::MeteoTypes;
        request(QUrl(QStringLiteral("%1/types").arg(QString::fromLatin1(kMeteoVisBase))),
                std::move(typesContext));
    }
    for (const auto* region : kMeteoRegions) {
        ++pending_;
        ++totalRequests_;
        RequestContext context;
        context.kind = RequestKind::MeteoRegions;
        context.meteoArg = QString::fromLatin1(region);
        QUrl url(QStringLiteral("%1/stream-buffers/live/hazards/%2")
                     .arg(QString::fromLatin1(kMeteoVisBase), context.meteoArg));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("language"), QStringLiteral("en"));
        query.addQueryItem(QStringLiteral("preset"), QStringLiteral("now"));
        query.addQueryItem(QStringLiteral("referenceDate"),
                           meteoReferenceDateString(meteoReferenceDate_));
        url.setQuery(query);
        request(url, std::move(context));
    }
    emit statusChanged(QStringLiteral("updating NWS/ECCC warnings, SPC reports, and Meteoalarm"));
}

void WeatherOverlaySource::pruneMeteoTileCache() {
    // Tile geometry is keyed by reference hour; drop everything from older
    // rounds so the cache tracks the current frame.
    const auto hourKey = meteoReferenceDateString(meteoReferenceDate_);
    for (auto it = meteoTilePolygons_.begin(); it != meteoTilePolygons_.end();) {
        if (!it.key().endsWith(hourKey)) it = meteoTilePolygons_.erase(it);
        else ++it;
    }
}

void WeatherOverlaySource::requestMeteoFollowUps() {
    if (!meteoReferenceDate_.isValid()) return;
    const auto now = QDateTime::currentDateTimeUtc();
    struct WantedDetail {
        QString key;
        QString featureId;
        int typeId = 0;
        int levelId = 0;
    };
    QVector<WantedDetail> wantedDetails;
    QSet<QString> warnedUuids;
    for (auto it = meteoHazardSets_.cbegin(); it != meteoHazardSets_.cend(); ++it) {
        for (const auto& hazard : it.value()) {
            warnedUuids.insert(hazard.featureId);
            const auto key = QStringLiteral("%1|%2|%3").arg(hazard.featureId).arg(hazard.typeId).arg(
                hazard.levelId);
            const auto fetched = meteoDetailFetchedAt_.value(key);
            if (meteoDetails_.contains(key) && fetched.isValid() &&
                fetched.secsTo(now) < 30 * 60) {
                continue;
            }
            if (meteoPendingDetails_.contains(key)) continue;
            wantedDetails.push_back({key, hazard.featureId, hazard.typeId, hazard.levelId});
        }
    }
    // Most severe first so a capped round still covers red/orange warnings.
    std::sort(wantedDetails.begin(), wantedDetails.end(), [](const auto& a, const auto& b) {
        if (a.levelId != b.levelId) return a.levelId < b.levelId;
        if (a.featureId != b.featureId) return a.featureId < b.featureId;
        return a.typeId < b.typeId;
    });
    while (wantedDetails.size() > kMeteoMaxNewDetails) wantedDetails.removeLast();
    for (const auto& wanted : wantedDetails) {
        meteoPendingDetails_.insert(wanted.key);
        ++pending_;
        ++totalRequests_;
        RequestContext context;
        context.kind = RequestKind::MeteoDetails;
        context.meteoArg = wanted.key;
        context.meteoFeature = wanted.featureId;
        context.meteoType = wanted.typeId;
        context.meteoLevel = wanted.levelId;
        QUrl url(QStringLiteral("%1/stream-buffers/live/features/%2/warnings")
                     .arg(QString::fromLatin1(kMeteoVisBase), wanted.featureId));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("language"), QStringLiteral("en"));
        query.addQueryItem(QStringLiteral("preset"), QStringLiteral("now"));
        query.addQueryItem(QStringLiteral("referenceDate"),
                           meteoReferenceDateString(meteoReferenceDate_));
        query.addQueryItem(QStringLiteral("typeId"), QString::number(wanted.typeId));
        query.addQueryItem(QStringLiteral("levelId"), QString::number(wanted.levelId));
        url.setQuery(query);
        request(url, std::move(context));
    }
    if (!meteoTilesAvailable() || warnedUuids.isEmpty()) return;
    // Covering z5 tiles for warned bboxes (deduped, bounded per round).
    const auto hourKey = meteoReferenceDateString(meteoReferenceDate_);
    QSet<QString> wantedTiles;
    for (auto it = meteoAreaSets_.cbegin(); it != meteoAreaSets_.cend(); ++it) {
        for (auto areaIt = it.value().cbegin(); areaIt != it.value().cend(); ++areaIt) {
            if (!warnedUuids.contains(areaIt.key())) continue;
            const auto& area = areaIt.value();
            for (const auto& tile : meteoTilesForBounds(area.minLon, area.minLat, area.maxLon,
                                                        area.maxLat)) {
                wantedTiles.insert(QStringLiteral("%1/%2/%3@%4")
                                       .arg(kMeteoTileZoom)
                                       .arg(tile.first)
                                       .arg(tile.second)
                                       .arg(hourKey));
            }
        }
    }
    QStringList ordered = wantedTiles.values();
    std::sort(ordered.begin(), ordered.end());
    int started = 0;
    for (const auto& tileKey : ordered) {
        if (meteoTilePolygons_.contains(tileKey) || meteoPendingTiles_.contains(tileKey)) continue;
        if (started >= kMeteoMaxNewTiles) break;
        ++started;
        meteoPendingTiles_.insert(tileKey);
        ++pending_;
        ++totalRequests_;
        const auto parts = tileKey.split(QLatin1Char('@')).first().split(QLatin1Char('/'));
        RequestContext context;
        context.kind = RequestKind::MeteoTiles;
        context.meteoArg = tileKey;
        context.meteoZ = parts.at(0).toInt();
        context.meteoX = parts.at(1).toInt();
        context.meteoY = parts.at(2).toInt();
        request(meteoTileUrl(context.meteoZ, context.meteoX, context.meteoY, meteoReferenceDate_),
                std::move(context));
    }
}

void WeatherOverlaySource::request(const QUrl& url, RequestContext context, const QByteArray& accept) {
    auto request = makeRequest(url, QString::fromUtf8(accept));
    // Conditional revalidation for every cached kind including zones: a
    // 304 serves the cached body with zero bytes (see requestFinished()).
    if (const auto etag = responseEtags_.value(url); !etag.isEmpty())
        request.setRawHeader(QByteArrayLiteral("If-None-Match"), etag);
    if (const auto modified = responseLastModified_.value(url); !modified.isEmpty())
        request.setRawHeader(QByteArrayLiteral("If-Modified-Since"), modified);
    auto* reply = network_.get(request);
    requests_.insert(reply, std::move(context));
}

void WeatherOverlaySource::requestFinished(QNetworkReply* reply) {
    const auto context = requests_.take(reply);
    const auto url = reply->request().url();
    const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray data;
    bool responseAvailable = false;
    if (statusCode == 304 && responseBodies_.contains(url)) {
        data = responseBodies_.value(url);
        responseAvailable = true;
        // A revalidation confirms the cached geometry: fresh again.
        if (context.kind == RequestKind::Zone && !context.zoneUrl.isEmpty())
            zoneFetchedAt_.insert(context.zoneUrl, QDateTime::currentSecsSinceEpoch());
    } else if (reply->error() == QNetworkReply::NoError) {
        data = reply->readAll();
        responseAvailable = true;
        responseBodies_.insert(url, data);
        if (const auto etag = reply->rawHeader(QByteArrayLiteral("ETag")); !etag.isEmpty())
            responseEtags_.insert(url, etag);
        if (const auto modified = reply->rawHeader(QByteArrayLiteral("Last-Modified")); !modified.isEmpty())
            responseLastModified_.insert(url, modified);
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
                const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
                for (int warningIndex = 0; warningIndex < warnings_.size(); ++warningIndex) {
                    if (!warnings_.at(warningIndex).polygons.isEmpty()) continue;
                    for (const auto& zoneUrl : warnings_.at(warningIndex).affectedZones) {
                        // Disk-warmed geometry under 7 days old skips its
                        // request entirely; stale entries revalidate with
                        // conditional headers instead of a full download.
                        if (zoneCache_.contains(zoneUrl) && zoneGeometryFresh(zoneUrl, nowSecs)) {
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
        } else if (context.kind == RequestKind::EcccAlerts) {
            auto parsed = parseEcccAlerts(data, &error);
            if (error.isEmpty()) {
                ecccWarnings_ = std::move(parsed);
                // Rides the NWS publish gate: merged at snapshot time, so no
                // gating changes and no map blanking when ECCC is slow.
                if (alertResponseReady_) schedulePublish();
            }
        } else if (context.kind == RequestKind::Reports) {
            auto parsed = parseStormReports(data, context.reportKind, &error, context.convectiveDate);
            if (error.isEmpty() && context.reportSet >= 0)
                reportSets_.at(static_cast<std::size_t>(context.reportSet)) = std::move(parsed);
        } else if (context.kind == RequestKind::MeteoTypes) {
            const auto document = QJsonDocument::fromJson(data, nullptr);
            if (document.isObject()) {
                for (const auto& value :
                     document.object().value(QStringLiteral("types")).toArray()) {
                    const auto type = value.toObject();
                    const auto id = type.value(QStringLiteral("id")).toInt(-1);
                    const auto label = type.value(QStringLiteral("label")).toString().trimmed();
                    if (id >= 0 && !label.isEmpty()) meteoTypeNames_.insert(QString::number(id), label);
                }
            } else {
                error = QStringLiteral("Invalid Meteoalarm types response");
            }
        } else if (context.kind == RequestKind::MeteoRegions) {
            const auto document = QJsonDocument::fromJson(data, nullptr);
            if (document.isObject()) {
                const auto root = document.object();
                QHash<QString, MeteoArea> areas;
                for (const auto& value : root.value(QStringLiteral("areas")).toArray()) {
                    const auto area = value.toObject();
                    const auto uuid = area.value(QStringLiteral("featureId")).toString();
                    if (uuid.isEmpty()) continue;
                    MeteoArea entry;
                    entry.name = area.value(QStringLiteral("name")).toString();
                    entry.region = context.meteoArg;
                    const auto box = area.value(QStringLiteral("bb")).toArray();
                    if (box.size() >= 2) {
                        const auto lo = box.at(0).toArray();
                        const auto hi = box.at(1).toArray();
                        if (lo.size() >= 2 && hi.size() >= 2) {
                            entry.minLon = lo.at(0).toDouble();
                            entry.minLat = lo.at(1).toDouble();
                            entry.maxLon = hi.at(0).toDouble();
                            entry.maxLat = hi.at(1).toDouble();
                        }
                    }
                    areas.insert(uuid, entry);
                }
                QVector<MeteoHazard> hazards;
                for (const auto& value : root.value(QStringLiteral("hazards")).toArray()) {
                    const auto hazard = value.toObject();
                    MeteoHazard entry;
                    entry.featureId = hazard.value(QStringLiteral("featureId")).toString();
                    entry.typeId = hazard.value(QStringLiteral("typeId")).toInt(0);
                    entry.levelId = hazard.value(QStringLiteral("levelId")).toInt(0);
                    if (!entry.featureId.isEmpty() && entry.levelId > 0) hazards.push_back(entry);
                }
                meteoAreaSets_.insert(context.meteoArg, std::move(areas));
                meteoHazardSets_.insert(context.meteoArg, std::move(hazards));
                requestMeteoFollowUps();
            } else {
                error = QStringLiteral("Invalid Meteoalarm hazards for %1").arg(context.meteoArg);
            }
        } else if (context.kind == RequestKind::MeteoDetails) {
            meteoPendingDetails_.remove(context.meteoArg);
            const auto document = QJsonDocument::fromJson(data, nullptr);
            const auto warnings = document.isObject()
                                      ? document.object().value(QStringLiteral("warnings")).toArray()
                                      : QJsonArray{};
            if (!document.isObject() || warnings.isEmpty()) {
                error = QStringLiteral("Invalid Meteoalarm details for %1").arg(context.meteoArg);
            } else {
                const auto detail = warnings.first().toObject();
                MeteoDetail entry;
                entry.warningId = detail.value(QStringLiteral("id")).toString();
                entry.headline = localizedText(detail.value(QStringLiteral("headline")));
                entry.description = localizedText(detail.value(QStringLiteral("description")));
                entry.instruction = localizedText(detail.value(QStringLiteral("instruction")));
                const auto originator = detail.value(QStringLiteral("originator")).toObject();
                entry.senderName = originator.value(QStringLiteral("name")).toString().trimmed();
                if (entry.senderName.isEmpty()) {
                    const auto region =
                        detail.value(QStringLiteral("regionCode")).toString().trimmed();
                    entry.senderName = region.isEmpty()
                                           ? QStringLiteral("Meteoalarm")
                                           : QStringLiteral("Meteoalarm/%1").arg(region);
                }
                entry.onset = meteoInstant(detail.value(QStringLiteral("startDate")).toString());
                entry.expires = meteoInstant(detail.value(QStringLiteral("endDate")).toString());
                meteoDetails_.insert(context.meteoArg, std::move(entry));
                meteoDetailFetchedAt_.insert(context.meteoArg, QDateTime::currentDateTimeUtc());
            }
        } else if (context.kind == RequestKind::MeteoTiles) {
            meteoPendingTiles_.remove(context.meteoArg);
            if (!meteoTilesAvailable()) {
                error = QStringLiteral("MVT tiles unavailable: build without zlib");
            } else {
                // Warned UUIDs at arrival time; tiles only carry geometry.
                QSet<QString> wanted;
                for (auto it = meteoHazardSets_.cbegin(); it != meteoHazardSets_.cend(); ++it) {
                    for (const auto& hazard : it.value()) wanted.insert(hazard.featureId);
                }
                QString tileError;
                const auto parsed =
                    parseMvtAreas(data, context.meteoZ, context.meteoX, context.meteoY, wanted,
                                  &tileError);
                if (!tileError.isEmpty()) {
                    error = QStringLiteral("Meteoalarm tile %1: %2").arg(context.meteoArg,
                                                                         tileError);
                } else {
                    meteoTilePolygons_.insert(context.meteoArg, std::move(parsed));
                }
            }
        } else if (context.kind == RequestKind::Zone) {
            auto polygons = parseZone(data, &error);
            if (error.isEmpty()) {
                zoneCache_.insert(context.zoneUrl, polygons);
                zoneFetchedAt_.insert(context.zoneUrl, QDateTime::currentSecsSinceEpoch());
                zoneCacheDirty_ = true;
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
    // One disk write per refresh round, only when zone geometry changed.
    if (pending_ == 0 && zoneCacheDirty_) {
        zoneCacheDirty_ = false;
        saveZoneDiskCache();
    }
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

QVector<WeatherWarning> WeatherOverlaySource::buildMeteoWarnings() const {
    // Merge cached tile geometry across tiles (deterministic order).
    QHash<QString, QVector<WarningPolygon>> geometry;
    auto tileKeys = meteoTilePolygons_.keys();
    std::sort(tileKeys.begin(), tileKeys.end());
    for (const auto& tileKey : tileKeys) {
        const auto& perTile = meteoTilePolygons_.value(tileKey);
        for (auto it = perTile.cbegin(); it != perTile.cend(); ++it)
            geometry[it.key()].append(it.value());
    }
    auto areaName = [this](const QString& uuid) -> QString {
        for (auto it = meteoAreaSets_.cbegin(); it != meteoAreaSets_.cend(); ++it) {
            const auto found = it.value().constFind(uuid);
            if (found != it.value().cend() && !found->name.isEmpty()) return found->name;
        }
        return {};
    };
    // Deterministic hazard order: country, then severity, then area.
    struct OrderedHazard {
        QString region;
        MeteoHazard hazard;
    };
    QVector<OrderedHazard> ordered;
    auto regions = meteoHazardSets_.keys();
    std::sort(regions.begin(), regions.end());
    for (const auto& region : regions) {
        for (const auto& hazard : meteoHazardSets_.value(region))
            ordered.push_back({region, hazard});
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.hazard.levelId != b.hazard.levelId) return a.hazard.levelId < b.hazard.levelId;
        if (a.region != b.region) return a.region < b.region;
        return a.hazard.featureId < b.hazard.featureId;
    });
    QSet<QString> emitted;
    QVector<WeatherWarning> warnings;
    for (const auto& item : ordered) {
        const auto& hazard = item.hazard;
        const auto detailKey = QStringLiteral("%1|%2|%3").arg(hazard.featureId).arg(hazard.typeId).arg(
            hazard.levelId);
        const auto detailIt = meteoDetails_.constFind(detailKey);
        if (detailIt == meteoDetails_.cend()) continue;  // details still in flight
        if (emitted.contains(detailIt->warningId.isEmpty() ? detailKey : detailIt->warningId))
            continue;
        emitted.insert(detailIt->warningId.isEmpty() ? detailKey : detailIt->warningId);
        const auto& detail = detailIt.value();
        WeatherWarning warning;
        warning.id = detail.warningId.isEmpty() ? QStringLiteral("meteoalarm:%1").arg(detailKey)
                                                : QStringLiteral("meteoalarm:%1").arg(detail.warningId);
        warning.event = meteoTypeNames_.value(QString::number(hazard.typeId),
                                              QStringLiteral("Weather warning"));
        const auto area = areaName(hazard.featureId);
        warning.headline = detail.headline.isEmpty()
                               ? (area.isEmpty() ? warning.event
                                                : QStringLiteral("%1 — %2").arg(area, warning.event))
                               : detail.headline;
        warning.severity = meteoSeverityForLevel(hazard.levelId);
        warning.senderName = detail.senderName;
        warning.description = detail.description;
        warning.instruction = detail.instruction;
        warning.onset = detail.onset;
        warning.expires = detail.expires;
        const auto style = meteoalarmLevelStyle(hazard.levelId);
        warning.alertColor = style.color;
        warning.alertPriority = style.priority;
        warning.polygons = geometry.value(hazard.featureId);
        warnings.push_back(std::move(warning));
    }
    return warnings;
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
    combined.append(ecccWarnings_);
    const auto meteoWarnings = buildMeteoWarnings();
    qsizetype meteoDrawable = 0;
    for (const auto& warning : meteoWarnings) {
        if (!warning.polygons.isEmpty()) ++meteoDrawable;
        combined.push_back(warning);
    }
    emit overlaysReady(combined, reports_, QDateTime::currentDateTimeUtc());
    if (pending_ != 0) return;
    if (failures_ == 0) {
        const auto drawable = std::count_if(combined.cbegin(), combined.cend(),
                                            [](const auto& warning) { return !warning.polygons.isEmpty(); });
        emit statusChanged(QStringLiteral("%1 drawable warnings (%2 NWS + %3 Meteoalarm) | %4 reports (24 h)")
                               .arg(drawable).arg(warnings_.size()).arg(meteoDrawable).arg(reports_.size()));
    } else {
        emit statusChanged(QStringLiteral("overlay update partial (%1 of %2 requests failed): %3")
                               .arg(failures_).arg(totalRequests_).arg(firstError_));
    }
}

} // namespace cappedweather
