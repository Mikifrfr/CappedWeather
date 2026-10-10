#include "app/nhc_cyclone.hpp"

#include "app/http_client.hpp"

#include <QNetworkReply>
#include <QRegularExpression>
#include <QXmlStreamReader>

#ifdef CAPPEDWEATHER_HAS_ZLIB
#include <zlib.h>
#endif

#include <algorithm>
#include <memory>
#include <vector>

namespace cappedweather {
namespace {

// KMZs are small (tens of KB); caps fail closed on hostile archives.
constexpr qsizetype kMaxKmzBytes = 8 * 1024 * 1024;
constexpr quint32 kMaxKmlBytes = 8 * 1024 * 1024;
constexpr int kMaxZipEntries = 64;
constexpr int kMaxStorms = 8;  // active cyclones rarely exceed a handful

bool readU16(const QByteArray& data, qsizetype& pos, quint16& value) {
    if (pos + 2 > data.size()) return false;
    value = static_cast<quint16>(static_cast<quint8>(data.at(pos)) |
                                 (static_cast<quint16>(static_cast<quint8>(data.at(pos + 1)))
                                  << 8));
    pos += 2;
    return true;
}

bool readU32(const QByteArray& data, qsizetype& pos, quint32& value) {
    if (pos + 4 > data.size()) return false;
    value = static_cast<quint32>(static_cast<quint8>(data.at(pos))) |
            (static_cast<quint32>(static_cast<quint8>(data.at(pos + 1))) << 8) |
            (static_cast<quint32>(static_cast<quint8>(data.at(pos + 2))) << 16) |
            (static_cast<quint32>(static_cast<quint8>(data.at(pos + 3))) << 24);
    pos += 4;
    return true;
}

// "lon,lat,alt lon,lat,alt ..." -> lon/lat points. Malformed triples are
// dropped, never fatal; altitude is ignored (sea-level overlay).
QVector<QPointF> parseLonLatList(const QString& text) {
    QVector<QPointF> points;
    for (const auto& triple : text.split(QRegularExpression(QStringLiteral("\\s+")),
                                         Qt::SkipEmptyParts)) {
        const auto parts = triple.split(QLatin1Char(','));
        if (parts.size() < 2) continue;
        bool okLon = false, okLat = false;
        const double lon = parts.at(0).toDouble(&okLon);
        const double lat = parts.at(1).toDouble(&okLat);
        if (!okLon || !okLat || lon < -180.0 || lon > 180.0 || lat < -90.0 || lat > 90.0)
            continue;
        points.push_back(QPointF(lon, lat));
    }
    return points;
}

}  // namespace

QByteArray extractKmlFromKmz(const QByteArray& kmz, QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QByteArray{};
    };
#ifndef CAPPEDWEATHER_HAS_ZLIB
    (void)kmz;
    return fail(QStringLiteral("NHC overlays need a CappedWeather build with zlib"));
#else
    if (kmz.isEmpty() || kmz.size() > kMaxKmzBytes)
        return fail(QStringLiteral("NHC archive is empty or oversized"));
    // Minimal local-header walk: parse each entry, inflate .kml payloads.
    qsizetype pos = 0;
    for (int entries = 0; entries < kMaxZipEntries; ++entries) {
        if (pos + 30 > kmz.size()) break;
        quint32 signature = 0;
        if (!readU32(kmz, pos, signature) || signature != 0x04034B50) break;
        quint16 version = 0, flags = 0, method = 0, modTime = 0, modDate = 0;
        quint32 crc = 0, compSize = 0, uncompSize = 0, nameLen = 0, extraLen = 0;
        quint16 nameLen16 = 0, extraLen16 = 0;
        if (!readU16(kmz, pos, version) || !readU16(kmz, pos, flags) ||
            !readU16(kmz, pos, method) || !readU16(kmz, pos, modTime) ||
            !readU16(kmz, pos, modDate) || !readU32(kmz, pos, crc) ||
            !readU32(kmz, pos, compSize) || !readU32(kmz, pos, uncompSize) ||
            !readU16(kmz, pos, nameLen16) || !readU16(kmz, pos, extraLen16))
            return fail(QStringLiteral("NHC archive header is truncated"));
        nameLen = nameLen16;
        extraLen = extraLen16;
        if (pos + static_cast<qsizetype>(nameLen + extraLen) > kmz.size())
            return fail(QStringLiteral("NHC archive name runs past the end"));
        const QString name =
            QString::fromUtf8(kmz.constData() + pos, static_cast<qsizetype>(nameLen));
        pos += static_cast<qsizetype>(nameLen + extraLen);
        if (pos + static_cast<qsizetype>(compSize) > kmz.size())
            return fail(QStringLiteral("NHC archive data runs past the end"));
        const bool want =
            name.endsWith(QStringLiteral(".kml"), Qt::CaseInsensitive) && uncompSize > 0 &&
            uncompSize <= kMaxKmlBytes && compSize <= static_cast<quint32>(kMaxKmzBytes);
        QByteArray payload;
        if (want) {
            if (method == 0) {
                if (compSize != uncompSize)
                    return fail(QStringLiteral("NHC stored entry has mismatched sizes"));
                payload = kmz.mid(pos, static_cast<qsizetype>(compSize));
            } else if (method == 8) {
                z_stream stream{};
                if (inflateInit2(&stream, -15) != Z_OK)
                    return fail(QStringLiteral("NHC inflate init failed"));
                payload.resize(static_cast<qsizetype>(uncompSize));
                stream.next_in =
                    reinterpret_cast<Bytef*>(const_cast<char*>(kmz.constData() + pos));
                stream.avail_in = compSize;
                stream.next_out = reinterpret_cast<Bytef*>(payload.data());
                stream.avail_out = uncompSize;
                const int result = inflate(&stream, Z_FINISH);
                const auto totalOut = stream.total_out;
                inflateEnd(&stream);
                if (result != Z_STREAM_END || totalOut != uncompSize)
                    return fail(QStringLiteral("NHC entry does not inflate cleanly"));
            } else {
                return fail(QStringLiteral("NHC archive uses an unsupported method"));
            }
            return payload;
        }
        pos += static_cast<qsizetype>(compSize);
    }
    return fail(QStringLiteral("NHC archive holds no KML file"));
#endif
}

QVector<NhcStormLink> parseNhcActiveKml(const QByteArray& data, QString* error) {
    QXmlStreamReader reader(data);
    QVector<NhcStormLink> links;
    QString folderName;
    QString coneUrl;
    QString trackUrl;
    int folderDepth = 0;
    bool inFolder = false;
    QString linkId;
    bool inLink = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const auto name = reader.name().toString();
            if (name == QStringLiteral("Folder")) {
                if (!inFolder) {
                    inFolder = true;
                    folderDepth = 1;
                    folderName.clear();
                    coneUrl.clear();
                    trackUrl.clear();
                } else {
                    ++folderDepth;
                }
            } else if (inFolder && folderDepth == 1 && name == QStringLiteral("name") &&
                       folderName.isEmpty()) {
                folderName = reader.readElementText();
            } else if (inFolder && name == QStringLiteral("NetworkLink")) {
                inLink = true;
                linkId = reader.attributes().value(QStringLiteral("id")).toString().toLower();
            } else if (inLink && name == QStringLiteral("href")) {
                const auto href = reader.readElementText().trimmed();
                if (linkId == QStringLiteral("cone")) coneUrl = href;
                else if (linkId == QStringLiteral("track")) trackUrl = href;
            }
        } else if (reader.isEndElement()) {
            const auto name = reader.name().toString();
            if (name == QStringLiteral("NetworkLink")) {
                inLink = false;
                linkId.clear();
            } else if (name == QStringLiteral("Folder") && inFolder) {
                if (--folderDepth == 0) {
                    inFolder = false;
                    if (!coneUrl.isEmpty() && !trackUrl.isEmpty()) {
                        static const QRegularExpression stormId(QStringLiteral("([A-Z]{2}\\d{6})"));
                        const auto match = stormId.match(coneUrl);
                        NhcStormLink link;
                        link.id = match.hasMatch() ? match.captured(1) : folderName;
                        link.name = folderName.isEmpty() ? link.id : folderName;
                        link.coneUrl = coneUrl;
                        link.trackUrl = trackUrl;
                        links.push_back(link);
                    }
                }
            }
        }
    }
    if (reader.hasError()) {
        if (error)
            *error = QStringLiteral("Invalid NHC feed: %1").arg(reader.errorString());
        return {};
    }
    if (error) error->clear();
    return links;
}

QVector<QPointF> parseNhcConeRing(const QByteArray& data, QString* error) {
    QXmlStreamReader reader(data);
    bool inPolygon = false;
    bool inCoordinates = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const auto name = reader.name().toString();
            if (name == QStringLiteral("Polygon")) inPolygon = true;
            else if (inPolygon && name == QStringLiteral("coordinates")) {
                inCoordinates = true;
                const auto points = parseLonLatList(reader.readElementText());
                if (points.size() >= 4) {
                    if (error) error->clear();
                    return points;
                }
                inCoordinates = false;
            }
        } else if (reader.isEndElement() && reader.name() == QStringLiteral("Polygon")) {
            inPolygon = false;
        }
    }
    if (reader.hasError()) {
        if (error)
            *error = QStringLiteral("Invalid NHC cone: %1").arg(reader.errorString());
        return {};
    }
    if (error) *error = QStringLiteral("NHC cone holds no drawable ring");
    return {};
}

QVector<QPointF> parseNhcTrackLine(const QByteArray& data, QString* error) {
    QXmlStreamReader reader(data);
    bool inLine = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const auto name = reader.name().toString();
            if (name == QStringLiteral("LineString")) inLine = true;
            else if (inLine && name == QStringLiteral("coordinates")) {
                const auto points = parseLonLatList(reader.readElementText());
                if (points.size() >= 2) {
                    if (error) error->clear();
                    return points;
                }
                inLine = false;
            }
        } else if (reader.isEndElement() && reader.name() == QStringLiteral("LineString")) {
            inLine = false;
        }
    }
    if (reader.hasError()) {
        if (error)
            *error = QStringLiteral("Invalid NHC track: %1").arg(reader.errorString());
        return {};
    }
    if (error) *error = QStringLiteral("NHC track holds no drawable line");
    return {};
}

NhcCycloneSource::NhcCycloneSource(QObject* parent) : QObject(parent) {}

void NhcCycloneSource::refresh() {
    if (!enabled_ || pending_) return;
    pending_ = true;
    auto* reply = network_.get(makeRequest(QUrl(QStringLiteral("https://www.nhc.noaa.gov/gis/kml/nhc.kmz")),
                                           QStringLiteral("application/vnd.google-earth.kmz"),
                                           QStringLiteral(" NHC cyclone client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            pending_ = false;
            emit statusChanged(
                QStringLiteral("NHC cyclones: feed failed (%1)").arg(reply->errorString()));
            return;
        }
        QString error;
        const auto kml = extractKmlFromKmz(reply->readAll(), &error);
        if (kml.isEmpty()) {
            pending_ = false;
            emit statusChanged(QStringLiteral("NHC cyclones: %1").arg(error));
            return;
        }
        auto links = parseNhcActiveKml(kml, &error);
        if (!error.isEmpty()) {
            pending_ = false;
            emit statusChanged(QStringLiteral("NHC cyclones: %1").arg(error));
            return;
        }
        requestProducts(std::move(links));
    });
}

void NhcCycloneSource::setEnabled(const bool enabled) {
    enabled_ = enabled;
}

void NhcCycloneSource::requestProducts(QVector<NhcStormLink> links) {
    if (links.size() > kMaxStorms) links.resize(kMaxStorms);
    if (links.isEmpty()) {
        pending_ = false;
        storms_.clear();
        emit cyclonesReady(storms_);
        emit statusChanged(QStringLiteral("NHC cyclones: none active"));
        return;
    }
    storms_.clear();
    struct Slot {
        NhcStormLink link;
        QVector<QPointF> cone;
        QVector<QPointF> track;
        bool coneDone = false;
        bool trackDone = false;
    };
    auto jobs = std::make_shared<std::vector<Slot>>();
    for (const auto& link : links) jobs->push_back(Slot{link});
    auto remaining = std::make_shared<int>(jobs->size() * 2);
    auto failed = std::make_shared<int>(0);
    auto finish = [this, jobs, remaining, failed] {
        if (--(*remaining) > 0) return;
        storms_.clear();
        for (const auto& slot : *jobs) {
            // Cone is the product; a track without a cone (or vice versa)
            // still draws what exists rather than dropping the storm.
            if (slot.cone.size() < 4 && slot.track.size() < 2) {
                ++(*failed);
                continue;
            }
            NhcStorm storm;
            storm.id = slot.link.id;
            storm.name = slot.link.name;
            storm.cone = slot.cone;
            storm.track = slot.track;
            storms_.push_back(std::move(storm));
        }
        std::sort(storms_.begin(), storms_.end(),
                  [](const NhcStorm& a, const NhcStorm& b) { return a.id < b.id; });
        pending_ = false;
        emit cyclonesReady(storms_);
        if (*failed > 0 && storms_.isEmpty())
            emit statusChanged(
                QStringLiteral("NHC cyclones: %1 failed to load").arg(*failed));
        else if (*failed > 0)
            emit statusChanged(QStringLiteral("NHC cyclones: %1 active (%2 unreadable)")
                                   .arg(storms_.size())
                                   .arg(*failed));
        else
            emit statusChanged(QStringLiteral("NHC cyclones: %1 active").arg(storms_.size()));
    };
    for (auto& slot : *jobs) {
        auto* coneReply = network_.get(makeRequest(QUrl(slot.link.coneUrl),
                                                   QStringLiteral("application/vnd.google-earth.kmz"),
                                                   QStringLiteral(" NHC cyclone client")));
        connect(coneReply, &QNetworkReply::finished, this,
                [coneReply, jobs, &slot, finish] {
                    coneReply->deleteLater();
                    if (coneReply->error() == QNetworkReply::NoError) {
                        QString error;
                        const auto kml = extractKmlFromKmz(coneReply->readAll(), &error);
                        if (!kml.isEmpty()) slot.cone = parseNhcConeRing(kml, &error);
                    }
                    slot.coneDone = true;
                    finish();
                });
        auto* trackReply = network_.get(makeRequest(QUrl(slot.link.trackUrl),
                                                    QStringLiteral("application/vnd.google-earth.kmz"),
                                                    QStringLiteral(" NHC cyclone client")));
        connect(trackReply, &QNetworkReply::finished, this,
                [trackReply, jobs, &slot, finish] {
                    trackReply->deleteLater();
                    if (trackReply->error() == QNetworkReply::NoError) {
                        QString error;
                        const auto kml = extractKmlFromKmz(trackReply->readAll(), &error);
                        if (!kml.isEmpty()) slot.track = parseNhcTrackLine(kml, &error);
                    }
                    slot.trackDone = true;
                    finish();
                });
    }
}

}  // namespace cappedweather
