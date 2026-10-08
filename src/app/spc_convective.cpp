#include "app/spc_convective.hpp"

#include "app/http_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimeZone>
#include <QXmlStreamReader>

#include <cmath>

namespace cappedweather {
namespace {

constexpr char kOutlookBase[] = "https://www.spc.noaa.gov/products/outlook/";
constexpr char kSpcBase[] = "https://www.spc.noaa.gov";
constexpr char kMcdRss[] = "https://www.spc.noaa.gov/products/spcmdrss.xml";
constexpr int kMaxMcdPages = 12;  // active MCDs rarely exceed a handful

QDateTime parseIso(const QString& text) {
    if (text.isEmpty()) return {};
    return QDateTime::fromString(text, Qt::ISODate);
}

QDateTime parseCompactUtc(const QString& text) {
    // SPC compact stamp YYYYMMDDHHMM (UTC).
    if (text.size() != 12) return {};
    bool ok = false;
    const int year = text.mid(0, 4).toInt(&ok);
    if (!ok) return {};
    const QDate date(year, text.mid(4, 2).toInt(), text.mid(6, 2).toInt());
    const QTime time(text.mid(8, 2).toInt(), text.mid(10, 2).toInt());
    if (!date.isValid() || !time.isValid()) return {};
    return QDateTime(date, time, QTimeZone::UTC);
}

int monthNumber(const QString& name) {
    static const QStringList months = {QStringLiteral("Jan"), QStringLiteral("Feb"),
                                       QStringLiteral("Mar"), QStringLiteral("Apr"),
                                       QStringLiteral("May"), QStringLiteral("Jun"),
                                       QStringLiteral("Jul"), QStringLiteral("Aug"),
                                       QStringLiteral("Sep"), QStringLiteral("Oct"),
                                       QStringLiteral("Nov"), QStringLiteral("Dec")};
    const int index = months.indexOf(name);
    return index >= 0 ? index + 1 : 0;
}

int tzOffsetHours(const QString& abbrev) {
    if (abbrev == QStringLiteral("EST")) return -5;
    if (abbrev == QStringLiteral("EDT")) return -4;
    if (abbrev == QStringLiteral("CST")) return -6;
    if (abbrev == QStringLiteral("CDT")) return -5;
    if (abbrev == QStringLiteral("MST")) return -7;
    if (abbrev == QStringLiteral("MDT")) return -6;
    if (abbrev == QStringLiteral("PST")) return -8;
    if (abbrev == QStringLiteral("PDT")) return -7;
    return 0;  // unknown zone: treat as UTC rather than dropping the MCD
}

QString stripHtml(const QByteArray& data) {
    QString text = QString::fromUtf8(data);
    // Drop scripts/styles whole (their code would otherwise read as text).
    static const QRegularExpression scripts(QStringLiteral("<(script|style)[^>]*>.*?</\\1>"),
                                            QRegularExpression::DotMatchesEverythingOption |
                                                QRegularExpression::CaseInsensitiveOption);
    text.remove(scripts);
    static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
    text.replace(tags, QStringLiteral("\n"));
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    return text;
}

QString section(const QString& text, const QString& start, const QString& end) {
    const QRegularExpression pattern(QRegularExpression::escape(start) +
                                     QStringLiteral("(.*?)") + QRegularExpression::escape(end),
                                     QRegularExpression::DotMatchesEverythingOption);
    const auto match = pattern.match(text);
    return match.hasMatch() ? match.captured(1).simplified() : QString{};
}

QDateTime mcdValidMoment(const QString& dayHourMin, const QDateTime& issueUtc) {
    // DDHHMMZ against the issue timestamp, with month rollover for windows
    // crossing midnight month boundaries (e.g. issued Jan 31, valid Feb 01).
    if (dayHourMin.size() != 6 || !issueUtc.isValid()) return {};
    bool ok = false;
    const int day = dayHourMin.mid(0, 2).toInt(&ok);
    if (!ok) return {};
    const QTime time(dayHourMin.mid(2, 2).toInt(), dayHourMin.mid(4, 2).toInt());
    if (!time.isValid()) return {};
    QDate date(issueUtc.date().year(), issueUtc.date().month(), 1);
    if (day >= 1 && day <= date.daysInMonth()) {
        date.setDate(date.year(), date.month(), day);
    } else {
        return {};
    }
    auto candidate = QDateTime(date, time, QTimeZone::UTC);
    const auto deltaDays = issueUtc.daysTo(candidate);
    if (deltaDays < -15) candidate = candidate.addMonths(1);
    else if (deltaDays > 15) candidate = candidate.addMonths(-1);
    return candidate;
}

}  // namespace

QVector<SpcOutlookFeature> parseSpcOutlookGeoJson(const QByteArray& data, const int day,
                                                  QString* error) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid SPC outlook GeoJSON: %1").arg(parseError.errorString());
        return {};
    }
    QVector<SpcOutlookFeature> features;
    for (const auto& value : document.object().value(QStringLiteral("features")).toArray()) {
        const auto feature = value.toObject();
        const auto geometry = feature.value(QStringLiteral("geometry")).toObject();
        const auto type = geometry.value(QStringLiteral("type")).toString();
        const auto coords = geometry.value(QStringLiteral("coordinates")).toArray();
        // Normalize Polygon -> [ring] and MultiPolygon -> [poly[ring]] to a
        // single list of exterior rings (first ring of each polygon).
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
        out.label = props.value(QStringLiteral("LABEL")).toString();
        out.label2 = props.value(QStringLiteral("LABEL2")).toString();
        const auto dnValue = props.value(QStringLiteral("DN"));
        out.dn = dnValue.isString() ? dnValue.toString().toInt() : dnValue.toInt();
        out.valid = parseIso(props.value(QStringLiteral("VALID_ISO")).toString());
        if (!out.valid.isValid()) out.valid = parseCompactUtc(props.value(QStringLiteral("VALID")).toString());
        out.expire = parseIso(props.value(QStringLiteral("EXPIRE_ISO")).toString());
        if (!out.expire.isValid())
            out.expire = parseCompactUtc(props.value(QStringLiteral("EXPIRE")).toString());
        out.issue = parseIso(props.value(QStringLiteral("ISSUE_ISO")).toString());
        if (!out.issue.isValid()) out.issue = parseCompactUtc(props.value(QStringLiteral("ISSUE")).toString());
        out.forecaster = props.value(QStringLiteral("FORECASTER")).toString();
        out.stroke = props.value(QStringLiteral("stroke")).toString();
        out.fill = props.value(QStringLiteral("fill")).toString();
        if (out.label.isEmpty() && out.dn <= 0) continue;
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

QVector<SpcMcdCandidate> parseMcdRss(const QByteArray& data, QString* error) {
    QXmlStreamReader reader(data);
    QVector<SpcMcdCandidate> candidates;
    QString title;
    QString link;
    bool inItem = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == QStringLiteral("item")) {
                inItem = true;
                title.clear();
                link.clear();
            } else if (inItem && reader.name() == QStringLiteral("title")) {
                title = reader.readElementText();
            } else if (inItem && reader.name() == QStringLiteral("link")) {
                link = reader.readElementText();
            }
        } else if (reader.isEndElement() && reader.name() == QStringLiteral("item")) {
            inItem = false;
            // "SPC - No MDs are in effect..." links back to the index: the
            // normal quiet state, not a discussion.
            if (title.startsWith(QStringLiteral("SPC - No "))) continue;
            static const QRegularExpression number(QStringLiteral("md(\\d+)\\.html"));
            const auto match = number.match(link);
            if (!match.hasMatch()) continue;
            SpcMcdCandidate candidate;
            candidate.number = match.captured(1).toInt();
            candidate.link = link;
            candidate.title = title;
            candidates.push_back(candidate);
        }
    }
    if (reader.hasError()) {
        if (error)
            *error = QStringLiteral("Invalid MCD RSS: %1").arg(reader.errorString());
        return {};
    }
    if (error) error->clear();
    return candidates;
}

SpcMcd parseMcdPage(const QByteArray& data, QString* error) {
    const QString text = stripHtml(data);
    static const QRegularExpression number(QStringLiteral("Mesoscale Discussion (\\d+)"));
    const auto numberMatch = number.match(text);
    if (!numberMatch.hasMatch()) {
        if (error) *error = QStringLiteral("No Mesoscale Discussion header found");
        return {};
    }
    SpcMcd mcd;
    mcd.number = numberMatch.captured(1).toInt();
    mcd.areas = section(text, QStringLiteral("Areas affected..."), QStringLiteral("Concerning..."));
    mcd.concerning = section(text, QStringLiteral("Concerning..."), QStringLiteral("Valid "));
    static const QRegularExpression valid(
        QStringLiteral("Valid (\\d{6})Z - (\\d{6})Z"));
    const auto validMatch = valid.match(text);
    static const QRegularExpression issued(
        QStringLiteral("(\\d{2})(\\d{2}) (AM|PM) ([A-Z]{2,4}) \\w{3} (\\w{3}) (\\d{2}) (\\d{4})"));
    const auto issuedMatch = issued.match(text);
    QDateTime issueUtc;
    if (issuedMatch.hasMatch()) {
        int hour = issuedMatch.captured(1).toInt();
        const int minute = issuedMatch.captured(2).toInt();
        const bool pm = issuedMatch.captured(3) == QStringLiteral("PM");
        if (pm && hour < 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
        const int month = monthNumber(issuedMatch.captured(5));
        const QDate date(issuedMatch.captured(7).toInt(), month,
                         issuedMatch.captured(6).toInt());
        if (month > 0 && date.isValid()) {
            const int offset = tzOffsetHours(issuedMatch.captured(4));
            issueUtc = QDateTime(date, QTime(hour, minute), QTimeZone::UTC).addSecs(-offset * 3600);
        }
    }
    if (validMatch.hasMatch()) {
        mcd.validFrom = mcdValidMoment(validMatch.captured(1), issueUtc);
        mcd.validTo = mcdValidMoment(validMatch.captured(2), issueUtc);
    }
    static const QRegularExpression probability(
        QStringLiteral("Probability of Watch Issuance\\.\\.\\.(\\d+) percent"));
    const auto probMatch = probability.match(text);
    if (probMatch.hasMatch()) mcd.watchProbabilityPercent = probMatch.captured(1).toInt();
    mcd.summary = section(text, QStringLiteral("SUMMARY..."), QStringLiteral("DISCUSSION..."));
    static const QRegularExpression discussion(
        QStringLiteral("DISCUSSION\\.\\.\\.(.*?)(?:\\.\\.[A-Za-z/]+\\.\\. \\d{2}/\\d{2}/\\d{4}|ATTN\\.\\.\\.|LAT\\.\\.\\.LON)"),
        QRegularExpression::DotMatchesEverythingOption);
    const auto discussionMatch = discussion.match(text);
    if (discussionMatch.hasMatch()) mcd.discussion = discussionMatch.captured(1).simplified();
    static const QRegularExpression latlon(QStringLiteral("LAT\\.\\.\\.LON((?:\\s*\\d{8})+)"));
    const auto latlonMatch = latlon.match(text);
    if (latlonMatch.hasMatch()) {
        static const QRegularExpression groups(QStringLiteral("\\d{8}"));
        auto it = groups.globalMatch(latlonMatch.captured(1));
        while (it.hasNext()) {
            const auto group = it.next().captured(0);
            // SPC convention: DDMM + DDDMM packed, longitude positive-west.
            const double lat = group.mid(0, 4).toDouble() / 100.0;
            const double lon = -group.mid(4, 4).toDouble() / 100.0;
            mcd.exterior.push_back(QPointF(lon, lat));
        }
    }
    if (error) error->clear();
    return mcd;
}

SpcConvectiveSource::SpcConvectiveSource(QObject* parent) : QObject(parent) {}

void SpcConvectiveSource::refresh() {
    if (!enabled_) return;
    fetchOutlooks();
    fetchMcds();
}

void SpcConvectiveSource::setEnabled(const bool enabled) {
    enabled_ = enabled;
}

void SpcConvectiveSource::fetchOutlooks() {
    if (outlooksPending_) return;
    outlooksPending_ = true;
    outlooks_.clear();
    struct Pending {
        int day = 0;
        QNetworkReply* reply = nullptr;
    };
    auto pending = std::make_shared<QVector<Pending>>();
    auto finishOne = [this, pending](int day, QNetworkReply* reply) {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const int code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (ok && code != 404) {
            QString error;
            const auto features = parseSpcOutlookGeoJson(reply->readAll(), day, &error);
            if (error.isEmpty()) outlooks_.append(features);
        }
        // 404 is SPC's normal "no outlook" state: quiet, not an error.
        reply->deleteLater();
        pending->erase(std::remove_if(pending->begin(), pending->end(),
                                      [reply](const Pending& p) { return p.reply == reply; }),
                       pending->end());
        if (pending->isEmpty()) {
            outlooksPending_ = false;
            emit outlooksReady(outlooks_);
            emit statusChanged(QStringLiteral("SPC outlooks: %1 areas").arg(outlooks_.size()));
        }
    };
    // One manager per source: this source must work without the warnings
    // source and must never share its conditional-cache state.
    for (const int day : {1, 2}) {
        const QUrl url(QString::fromLatin1(kOutlookBase) +
                       QStringLiteral("day%1otlk_cat.nolyr.geojson").arg(day));
        auto* reply = network_.get(makeRequest(url, QStringLiteral("application/geo+json"),
                                               QStringLiteral(" SPC outlook client")));
        pending->push_back({day, reply});
        connect(reply, &QNetworkReply::finished, this,
                [finishOne, day, reply] { finishOne(day, reply); });
    }
}

void SpcConvectiveSource::fetchMcds() {
    if (mcdsPending_) return;
    mcdsPending_ = true;
    auto* reply = network_.get(makeRequest(QUrl(QString::fromLatin1(kMcdRss)),
                                           QStringLiteral("application/rss+xml"),
                                           QStringLiteral(" SPC MCD client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            mcdsPending_ = false;
            emit statusChanged(QStringLiteral("SPC MCDs: feed failed (%1)")
                                   .arg(reply->errorString()));
            return;
        }
        QString error;
        const auto candidates = parseMcdRss(reply->readAll(), &error);
        if (!error.isEmpty()) {
            mcdsPending_ = false;
            emit statusChanged(QStringLiteral("SPC MCDs: %1").arg(error));
            return;
        }
        requestMcdPages(candidates);
    });
}

void SpcConvectiveSource::requestMcdPages(QVector<SpcMcdCandidate> candidates) {
    if (candidates.size() > kMaxMcdPages) candidates.resize(kMaxMcdPages);
    if (candidates.isEmpty()) {
        mcdsPending_ = false;
        mcds_.clear();
        emit mcdsReady(mcds_);
        emit statusChanged(QStringLiteral("SPC MCDs: none in effect"));
        return;
    }
    mcds_.clear();
    auto remaining = std::make_shared<int>(candidates.size());
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto& candidate : candidates) {
        QUrl url(candidate.link);
        if (url.isRelative())
            url = QUrl(QString::fromLatin1(kSpcBase) + candidate.link);
        auto* reply = network_.get(makeRequest(url, QStringLiteral("text/html"),
                                               QStringLiteral(" SPC MCD client")));
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, remaining, now] {
                    reply->deleteLater();
                    if (reply->error() == QNetworkReply::NoError) {
                        QString error;
                        auto mcd = parseMcdPage(reply->readAll(), &error);
                        // Keep only live discussions with a drawable polygon.
                        if (error.isEmpty() && mcd.exterior.size() >= 4 &&
                            (!mcd.validTo.isValid() || mcd.validTo > now)) {
                            mcds_.push_back(std::move(mcd));
                        }
                    }
                    if (--(*remaining) == 0) {
                        mcdsPending_ = false;
                        std::sort(mcds_.begin(), mcds_.end(),
                                  [](const SpcMcd& a, const SpcMcd& b) { return a.number < b.number; });
                        emit mcdsReady(mcds_);
                        emit statusChanged(QStringLiteral("SPC MCDs: %1 active").arg(mcds_.size()));
                    }
                });
    }
}

}  // namespace cappedweather
