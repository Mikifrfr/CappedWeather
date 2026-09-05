#include "app/opera_source.hpp"

#include "app/http_client.hpp"
#include "app/s3_listing.hpp"
#include "core/odim_reader.hpp"
#include "core/radar_catalog.hpp"

#include <QMetaObject>
#include <QPair>
#include <QRegularExpression>
#include <QSettings>
#include <QTimeZone>
#include <QUrlQuery>

#include <algorithm>
#include <span>

namespace cappedweather {

namespace {

constexpr int kListingPageSize = 100;
constexpr int kMaxActiveDownloads = 3;
constexpr char kBucketHost[] = "s3.waw3-1.cloudferro.com";
constexpr char kBucket[] = "openradar-24h";
constexpr char kUserAgentSuffix[] = " OPERA ODIM client";

QString findOdimSite(const QString& radarId, QString* odimCode, QString* countryCode) {
    // Cheap linear scan over 179 sites, cached after first use.
    static const std::vector<RadarSite> sites = RadarCatalog::operaSites();
    const std::string wanted = radarId.toUpper().toStdString();
    for (const auto& site : sites) {
        if (site.id == wanted) {
            if (odimCode != nullptr) *odimCode = QString::fromStdString(site.odimCode);
            if (countryCode != nullptr) *countryCode = QString::fromStdString(site.countryCode);
            return QString::fromStdString(site.id);
        }
    }
    return {};
}

} // namespace

OperaSource::OperaSource(QObject* parent) : QObject(parent) {
    processingPool_.setMaxThreadCount(qMax(1, QThreadPool::globalInstance()->maxThreadCount() / 2));
    connect(&network_, &QNetworkAccessManager::finished, this, [this](QNetworkReply* reply) {
        if (listingRequests_.contains(reply)) finishListing(reply);
        else if (downloadRequests_.contains(reply)) finishObject(reply);
        else reply->deleteLater();
    });
}

QString OperaSource::pvolPrefix(const QString& countryCode, const QString& odimCode,
                                const QDate& date) {
    return QStringLiteral("%1/%2/%3/%4/%5/PVOL/")
        .arg(date.toString(QStringLiteral("yyyy")),
             date.toString(QStringLiteral("MM")), date.toString(QStringLiteral("dd")),
             countryCode, odimCode);
}

QDateTime OperaSource::timestampFromKey(const QString& key) {
    // "<odim>@YYYYMMDDTHHMM@<elevations>@<MOMENT>.h5"
    static const QRegularExpression pattern(
        QStringLiteral("@(\\d{4})(\\d{2})(\\d{2})T(\\d{2})(\\d{2})@"));
    const auto match = pattern.match(key);
    if (!match.hasMatch()) return {};
    const QDate date(match.captured(1).toInt(), match.captured(2).toInt(),
                     match.captured(3).toInt());
    const QTime time(match.captured(4).toInt(), match.captured(5).toInt());
    if (!date.isValid() || !time.isValid()) return {};
    return QDateTime(date, time, QTimeZone::UTC);
}

bool OperaSource::isDbzhKey(const QString& key, const QString& odimCode) {
    const auto file = key.section(QLatin1Char('/'), -1);
    return file.startsWith(odimCode + QStringLiteral("@"), Qt::CaseInsensitive) &&
           file.endsWith(QStringLiteral("@DBZH.h5"), Qt::CaseInsensitive);
}

QUrl OperaSource::listingUrl(const QString& prefix, const QString& startAfter) {
    QUrl url(QStringLiteral("https://%1/%2").arg(QString::fromLatin1(kBucketHost),
                                                 QString::fromLatin1(kBucket)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("list-type"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("prefix"), prefix);
    query.addQueryItem(QStringLiteral("max-keys"), QString::number(kListingPageSize));
    if (!startAfter.isEmpty()) query.addQueryItem(QStringLiteral("start-after"), startAfter);
    url.setQuery(query);
    return url;
}

QUrl OperaSource::objectUrl(const QString& key) {
    return QUrl(QStringLiteral("https://%1/%2/%3").arg(QString::fromLatin1(kBucketHost),
                                                       QString::fromLatin1(kBucket), key));
}

int OperaSource::maxAttempts() {
    return QSettings().value(QStringLiteral("network/retries"), 2).toInt() + 1;
}

int OperaSource::retryBackoffMs(const int attempt) {
    const auto base =
        QSettings().value(QStringLiteral("network/retryBackoffMs"), 250).toInt();
    return std::clamp(base, 100, 5000) * (1 << std::min(attempt, 4));
}

void OperaSource::requestLatest(const QString& radarId) {
    QString odimCode;
    QString countryCode;
    if (findOdimSite(radarId, &odimCode, &countryCode).isEmpty()) {
        failRadar(radarId, QStringLiteral("Unknown OPERA station %1").arg(radarId));
        return;
    }
    cancelledRadars_.remove(radarId);
    if (inFlightRadars_.contains(radarId)) return;
    inFlightRadars_.insert(radarId);
    const auto today = QDateTime::currentDateTimeUtc().date();
    // Date rollover: yesterday's objects are still today's newest volumes.
    startListing({radarId, odimCode, countryCode, {today, today.addDays(-1)}, 0, 1, {}});
}

void OperaSource::requestRecent(const QString& radarId, int volumeCount) {
    QString odimCode;
    QString countryCode;
    if (findOdimSite(radarId, &odimCode, &countryCode).isEmpty()) {
        failRadar(radarId, QStringLiteral("Unknown OPERA station %1").arg(radarId));
        return;
    }
    cancelledRadars_.remove(radarId);
    if (inFlightRadars_.contains(radarId)) return;
    inFlightRadars_.insert(radarId);
    const auto today = QDateTime::currentDateTimeUtc().date();
    startListing({radarId, odimCode, countryCode,
                  {today, today.addDays(-1)}, 0, qMax(1, volumeCount), {}});
}

void OperaSource::clearDownloaded(const QString& radarId) {
    downloadedVolumeIds_.remove(radarId);
}

void OperaSource::cancel(const QString& radarId) {
    cancelledRadars_.insert(radarId);
    inFlightRadars_.remove(radarId);
    pendingBacklog_.remove(radarId);
    for (auto it = listingRequests_.begin(); it != listingRequests_.end();) {
        if (it.value().radarId == radarId) {
            auto* reply = it.key();
            it = listingRequests_.erase(it);
            reply->abort();
        } else {
            ++it;
        }
    }
    for (auto it = downloadRequests_.begin(); it != downloadRequests_.end();) {
        if (it.value().radarId == radarId) {
            auto* reply = it.key();
            it = downloadRequests_.erase(it);
            reply->abort();
        } else {
            ++it;
        }
    }
}

void OperaSource::startListing(ListingContext context) {
    const auto prefix = pvolPrefix(context.countryCode, context.odimCode,
                                   context.dates.at(context.dateIndex));
    emit statusChanged(context.radarId,
                       QStringLiteral("OPERA listing %1").arg(context.radarId));
    auto* reply = network_.get(makeRequest(listingUrl(prefix, context.startAfter), {},
                                           QString::fromLatin1(kUserAgentSuffix)));
    listingRequests_.insert(reply, std::move(context));
}

void OperaSource::finishListing(QNetworkReply* reply) {
    ListingContext context = listingRequests_.take(reply);
    const auto finish = [reply] { reply->deleteLater(); };
    if (cancelledRadars_.contains(context.radarId)) {
        inFlightRadars_.remove(context.radarId);
        finish();
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        // Try the next date before giving up (today may be empty); keep
        // whatever this date already contributed.
        if (context.dateIndex + 1 < context.dates.size()) {
            ++context.dateIndex;
            context.startAfter.clear();
            finish();
            startListing(std::move(context));
            return;
        }
        inFlightRadars_.remove(context.radarId);
        const auto message = reply->errorString();
        finish();
        failRadar(context.radarId, QStringLiteral("OPERA listing failed: %1").arg(message));
        return;
    }
    QString error;
    const auto parsed = parseS3Objects(reply->readAll(), &error);
    finish();
    for (const auto& object : parsed.objects) {
        if (!isDbzhKey(object.key, context.odimCode)) continue;
        const auto stamp = timestampFromKey(object.key);
        if (!stamp.isValid()) continue;
        context.candidates.push_back({object.key, stamp});
    }
    if (parsed.truncated) {
        context.startAfter = parsed.lastKey;
        startListing(std::move(context));
        return;
    }
    // One date exhausted: fall through to the next before deciding.
    if (context.dateIndex + 1 < context.dates.size()) {
        ++context.dateIndex;
        context.startAfter.clear();
        startListing(std::move(context));
        return;
    }
    std::sort(context.candidates.begin(), context.candidates.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    // Newest-first, skipping volumes already delivered in this session.
    QVector<QPair<QString, QDateTime>> fresh;
    auto& seen = downloadedVolumeIds_[context.radarId];
    for (const auto& candidate : context.candidates) {
        const auto volumeId = candidate.second.toString(QStringLiteral("yyyyMMddTHHmm"));
        if (seen.contains(volumeId)) continue;
        if (!fresh.contains(candidate)) fresh.push_back(candidate);
        if (fresh.size() >= static_cast<std::size_t>(context.requestedCount)) break;
    }
    if (fresh.isEmpty()) {
        inFlightRadars_.remove(context.radarId);
        emit statusChanged(context.radarId, QStringLiteral("OPERA %1 up to date")
                                                     .arg(context.radarId));
        return;
    }
    beginDownloads(context.radarId, std::move(fresh), context.requestedCount);
}

void OperaSource::beginDownloads(const QString& radarId,
                                 QVector<QPair<QString, QDateTime>> candidates,
                                 int requestedCount) {
    Q_UNUSED(requestedCount);
    int started = 0;
    while (!candidates.isEmpty() && started < kMaxActiveDownloads) {
        const auto candidate = candidates.takeFirst();
        const auto volumeId = candidate.second.toString(QStringLiteral("yyyyMMddTHHmm"));
        downloadedVolumeIds_[radarId].insert(volumeId);
        emit volumeStarted(radarId, volumeId, 1);
        requestObject({radarId, volumeId, candidate.first, candidate.second, 0});
        ++started;
    }
    // The remainder follow as downloads complete (see completeVolume).
    if (!candidates.isEmpty()) {
        pendingBacklog_[radarId].append(candidates);
    }
}

void OperaSource::requestObject(DownloadContext context) {
    emit statusChanged(context.radarId, QStringLiteral("OPERA downloading %1 %2")
                                                 .arg(context.radarId, context.volumeId));
    auto* reply = network_.get(
        makeRequest(objectUrl(context.key), {}, QString::fromLatin1(kUserAgentSuffix)));
    downloadRequests_.insert(reply, std::move(context));
}

void OperaSource::finishObject(QNetworkReply* reply) {
    DownloadContext context = downloadRequests_.take(reply);
    if (cancelledRadars_.contains(context.radarId)) {
        inFlightRadars_.remove(context.radarId);
        reply->deleteLater();
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (context.attempt + 1 < maxAttempts()) {
            ++context.attempt;
            const auto delay = retryBackoffMs(context.attempt);
            reply->deleteLater();
            QTimer::singleShot(delay, this, [this, context]() {
                if (!cancelledRadars_.contains(context.radarId)) requestObject(context);
            });
            return;
        }
        const auto message = reply->errorString();
        reply->deleteLater();
        completeVolume(context, QDateTime::currentDateTimeUtc(), std::nullopt,
                       QStringLiteral("OPERA download failed: %1").arg(message));
        return;
    }
    const auto payload = reply->readAll();
    reply->deleteLater();
    decodeObject(context, payload);
}

void OperaSource::decodeObject(DownloadContext context, QByteArray payload) {
    const QPointer<OperaSource> guarded(this);
    processingPool_.start([guarded, context, payload]() mutable {
        std::string error;
        const auto bytes = std::span(reinterpret_cast<const std::byte*>(payload.constData()),
                                     static_cast<std::size_t>(payload.size()));
        auto decoded = readOdimPvol(bytes, context.radarId.toStdString(), &error);
        const auto failure = decoded.has_value()
                                 ? QString()
                                 : QStringLiteral("OPERA decode failed: %1").arg(
                                       QString::fromStdString(error));
        QMetaObject::invokeMethod(
            guarded,
            [guarded, context, decoded = std::move(decoded), failure]() mutable {
                if (guarded.isNull() || guarded->cancelledRadars_.contains(context.radarId)) {
                    return;
                }
                guarded->completeVolume(context, QDateTime::currentDateTimeUtc(),
                                        std::move(decoded), failure);
            });
    });
}

void OperaSource::completeVolume(DownloadContext context, const QDateTime& receivedAt,
                                 std::optional<DecodedChunk> decoded, const QString& failure) {
    const bool ok = decoded.has_value();
    if (ok) {
        emit chunkDecoded(context.radarId, context.volumeId, context.key, *decoded, receivedAt);
    }
    emit volumeProgress(context.radarId, context.volumeId, 1, 1, ok ? 1 : 0, ok ? 0 : 1);
    emit volumeReady(context.radarId, context.volumeId, 1, ok ? 0 : 1, receivedAt);
    if (!ok && !failure.isEmpty()) emit statusChanged(context.radarId, failure);
    // Drain any backlog queued by beginDownloads, then release the radar.
    auto backlog = pendingBacklog_.take(context.radarId);
    int started = 0;
    while (!backlog.isEmpty() && started < kMaxActiveDownloads) {
        const auto candidate = backlog.takeFirst();
        const auto volumeId = candidate.second.toString(QStringLiteral("yyyyMMddTHHmm"));
        downloadedVolumeIds_[context.radarId].insert(volumeId);
        emit volumeStarted(context.radarId, volumeId, 1);
        requestObject({context.radarId, volumeId, candidate.first, candidate.second, 0});
        ++started;
    }
    if (!backlog.isEmpty()) {
        pendingBacklog_[context.radarId] = backlog;
        return;
    }
    // Release the radar once none of its downloads remain in flight.
    for (const auto& pending : downloadRequests_) {
        if (pending.radarId == context.radarId) return;
    }
    inFlightRadars_.remove(context.radarId);
}

void OperaSource::failRadar(const QString& radarId, const QString& message) {
    inFlightRadars_.remove(radarId);
    emit requestFailed(radarId, message);
}

} // namespace cappedweather
