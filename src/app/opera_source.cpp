#include "app/opera_source.hpp"

#include "app/http_client.hpp"
#include "app/s3_listing.hpp"
#include "core/odim_reader.hpp"
#include "core/radar_catalog.hpp"

#include <QMetaObject>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QSettings>
#include <QTimeZone>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace cappedweather {

namespace {

constexpr int kListingPageSize = 1000;
constexpr int kMaxActiveDownloads = 3;
constexpr char kBucketHost[] = "s3.waw3-1.cloudferro.com";
constexpr char kBucket[] = "openradar-24h";
constexpr char kUserAgentSuffix[] = " OPERA ODIM client";

QString findOdimSite(const QString& radarId, QString* odimCode, QString* countryCode) {
    static const std::vector<RadarSite> sites = RadarCatalog::operaSites();
    // O(1) id -> site cache built once on first use (function-local static
    // init is thread-safe); avoids a linear scan per requestLatest/requestRecent.
    static const auto byId = [&]() {
        std::unordered_map<std::string, const RadarSite*> index;
        index.reserve(sites.size() * 2);
        for (const auto& site : sites) index.emplace(site.id, &site);
        return index;
    }();
    const std::string wanted = radarId.toUpper().toStdString();
    const auto found = byId.find(wanted);
    if (found == byId.end()) return {};
    if (odimCode != nullptr) *odimCode = QString::fromStdString(found->second->odimCode);
    if (countryCode != nullptr) *countryCode = QString::fromStdString(found->second->countryCode);
    return QString::fromStdString(found->second->id);
}

} // namespace

OperaSource::OperaSource(QObject* parent) : QObject(parent) {
    qRegisterMetaType<std::shared_ptr<cappedweather::DecodedChunk>>();
    processingPool_.setMaxThreadCount(
        std::clamp(QThreadPool::globalInstance()->maxThreadCount(), 2, 8));
    attachChunkDiskCache(network_, QStringLiteral("opera"));
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

QString OperaSource::scanPrefix(const QString& countryCode, const QString& odimCode,
                                const QDate& date) {
    return QStringLiteral("%1/%2/%3/%4/%5/SCAN/")
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
    if (!file.startsWith(odimCode + QStringLiteral("@"), Qt::CaseInsensitive)) return false;
    // Moment lists vary by provider: "@DBZH.h5" (separate objects) as well as
    // combined single objects such as "@DBZH_TH_VRADH.h5" (FI/SE/FR/DE-SCAN)
    // and "@CCORH_DBZH_PHIDP_RHOHV_SQIH_TH_VRADH_WRADH_ZDR.h5" (NL PVOL).
    return file.toUpper().contains(QStringLiteral("DBZH"));
}

bool OperaSource::isThKey(const QString& key, const QString& odimCode) {
    const auto file = key.section(QLatin1Char('/'), -1);
    if (!file.startsWith(odimCode + QStringLiteral("@"), Qt::CaseInsensitive)) return false;
    return file.toUpper().contains(QStringLiteral("TH"));
}

QVector<QVector<QPair<QString, QDateTime>>> OperaSource::groupScanCandidates(
    const QVector<QPair<QString, QDateTime>>& newestFirst, int requestedCount,
    int maxPerGroup) {
    QVector<QVector<QPair<QString, QDateTime>>> groups;
    if (requestedCount <= 0) return groups;
    // SCAN providers sweep elevations across several minutes (FR spreads a
    // volume over ~5 min); bucket by 5-minute scan cycle so one bucket holds
    // one volume's elevations regardless of provider staggering.
    QMap<QString, QVector<QPair<QString, QDateTime>>> buckets;
    for (const auto& candidate : newestFirst) {
        const auto stamp = candidate.second;
        const QDateTime bucketTime(
            stamp.date(), QTime(stamp.time().hour(), (stamp.time().minute() / 5) * 5, 0),
            QTimeZone::UTC);
        buckets[bucketTime.toString(QStringLiteral("yyyyMMddTHHmm"))].push_back(candidate);
    }
    auto bucketKeys = buckets.keys();
    std::sort(bucketKeys.begin(), bucketKeys.end(),
              [](const QString& a, const QString& b) { return a > b; });
    for (const auto& bucketKey : bucketKeys) {
        if (groups.size() >= requestedCount) break;
        auto files = buckets.value(bucketKey);
        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        // Dedupe identical keys (listing overlaps across date prefixes).
        QVector<QPair<QString, QDateTime>> unique;
        for (const auto& file : files) {
            bool seen = false;
            for (const auto& kept : unique) {
                if (kept.first == file.first) {
                    seen = true;
                    break;
                }
            }
            if (!seen) unique.push_back(file);
            if (unique.size() >= static_cast<std::size_t>(maxPerGroup)) break;
        }
        if (!unique.isEmpty()) groups.push_back(std::move(unique));
    }
    return groups;
}

QVector<QVector<QPair<QString, QDateTime>>> OperaSource::trimScanGroups(
    QVector<QVector<QPair<QString, QDateTime>>> groups, int requestedCount,
    const QDateTime& now) {
    // Ask for one spare bucket upstream so a filling newest bucket can be
    // skipped in favor of the previous complete one.
    if (groups.size() > 1) {
        const auto& newest = groups.front();
        const auto& previous = groups.at(1);
        QDateTime newestFile = newest.front().second;
        for (const auto& file : newest) newestFile = std::max(newestFile, file.second);
        const auto ageSecs = newestFile.secsTo(now);
        if (newest.size() < previous.size() && ageSecs >= 0 && ageSecs < 8 * 60) {
            groups.removeFirst();
        }
    }
    while (groups.size() > requestedCount && !groups.isEmpty()) groups.removeLast();
    return groups;
}

DecodedChunk OperaSource::mergeScanParts(std::vector<DecodedChunk> parts,
                                         const std::string& radarId) {    DecodedChunk merged;
    merged.radarId = radarId;
    // Parts come from the same ODIM file, but a merge that sees two different
    // conventions must report Unknown rather than pick one.
    if (!parts.empty()) merged.momentEncoding = parts.front().momentEncoding;
    std::vector<DecodedRay> all;
    for (auto& part : parts) {
        if (part.momentEncoding != merged.momentEncoding) {
            merged.momentEncoding = MomentEncoding::Unknown;
        }
        merged.messageCount += part.messageCount;
        merged.rejectedMessages += part.rejectedMessages;
        merged.rejectedBlocks += part.rejectedBlocks;
        for (auto& ray : part.rays) all.push_back(std::move(ray));
    }
    if (all.empty()) return merged;
    // Unique elevations within 0.05 deg tolerance, sorted ascending.
    std::vector<float> elevations;
    elevations.reserve(all.size());
    for (const auto& ray : all) elevations.push_back(ray.elevationDegrees);
    std::sort(elevations.begin(), elevations.end());
    std::vector<float> unique;
    for (const auto value : elevations) {
        if (unique.empty() || std::fabs(value - unique.back()) > 0.05F) unique.push_back(value);
    }
    for (auto& ray : all) {
        std::size_t best = 0;
        float bestDistance = std::fabs(ray.elevationDegrees - unique.front());
        for (std::size_t index = 1; index < unique.size(); ++index) {
            const float distance = std::fabs(ray.elevationDegrees - unique[index]);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = index;
            }
        }
        ray.elevationNumber = static_cast<std::uint8_t>(std::min<std::size_t>(best + 1, 255));
    }
    std::sort(all.begin(), all.end(), [](const DecodedRay& a, const DecodedRay& b) {
        if (a.elevationNumber != b.elevationNumber) return a.elevationNumber < b.elevationNumber;
        return a.azimuthDegrees < b.azimuthDegrees;
    });
    merged.rays = std::move(all);
    return merged;
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
    const auto yesterday = today.addDays(-1);
    // Date rollover: yesterday's objects are still today's newest volumes.
    // Providers disagree on container: PVOL (multi-tilt, e.g. NO/NL) vs SCAN
    // (single elevation, e.g. FR/DE/FI/SE). List both; PVOL wins when present.
    ListingContext context;
    context.radarId = radarId;
    context.odimCode = odimCode;
    context.countryCode = countryCode;
    context.prefixes = {pvolPrefix(countryCode, odimCode, today),
                        pvolPrefix(countryCode, odimCode, yesterday),
                        scanPrefix(countryCode, odimCode, today),
                        scanPrefix(countryCode, odimCode, yesterday)};
    context.prefixIndex = 0;
    context.requestedCount = 1;
    startListing(std::move(context));
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
    const auto yesterday = today.addDays(-1);
    ListingContext context;
    context.radarId = radarId;
    context.odimCode = odimCode;
    context.countryCode = countryCode;
    context.prefixes = {pvolPrefix(countryCode, odimCode, today),
                        pvolPrefix(countryCode, odimCode, yesterday),
                        scanPrefix(countryCode, odimCode, today),
                        scanPrefix(countryCode, odimCode, yesterday)};
    context.prefixIndex = 0;
    context.requestedCount = qMax(1, volumeCount);
    startListing(std::move(context));
}

void OperaSource::clearDownloaded(const QString& radarId) {
    downloadedVolumeIds_.remove(radarId);
}

void OperaSource::cancel(const QString& radarId) {
    cancelledRadars_.insert(radarId);
    inFlightRadars_.remove(radarId);
    pendingBacklog_.remove(radarId);
    pendingScanVolumes_.remove(radarId);
    activeScanGroups_.remove(radarId);
    // No leak: abort() still emits finished, which routes to deleteLater via
    // the finished lambda in the constructor (erased entries hit its else
    // branch; retained entries hit finishListing/finishObject's deleteLater).
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
    if (context.prefixIndex < 0 || context.prefixIndex >= context.prefixes.size()) return;
    const auto prefix = context.prefixes.at(context.prefixIndex);
    emit statusChanged(context.radarId,
                       QStringLiteral("OPERA listing %1").arg(context.radarId));
    auto* reply = network_.get(makeRequest(listingUrl(prefix, context.startAfter), {},
                                           QString::fromLatin1(kUserAgentSuffix),
                                           CacheMode::AlwaysNetwork));
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
    const auto advancePrefix = [this](ListingContext moved) {
        ++moved.prefixIndex;
        moved.startAfter.clear();
        startListing(std::move(moved));
    };
    if (reply->error() != QNetworkReply::NoError) {
        // Try the next prefix before giving up; keep whatever already
        // contributed.
        if (context.prefixIndex + 1 < context.prefixes.size()) {
            finish();
            advancePrefix(std::move(context));
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
        const auto stamp = timestampFromKey(object.key);
        if (!stamp.isValid()) continue;
        if (isDbzhKey(object.key, context.odimCode)) {
            if (!context.seenKeys.contains(object.key)) {
                context.seenKeys.insert(object.key);
                context.candidates.push_back({object.key, stamp});
            }
        } else if (isThKey(object.key, context.odimCode)) {
            // TH-only fallback for sites/moments with no DBZH object.
            if (!context.seenThKeys.contains(object.key)) {
                context.seenThKeys.insert(object.key);
                context.thCandidates.push_back({object.key, stamp});
            }
        }
    }
    if (parsed.truncated) {
        context.startAfter = parsed.lastKey;
        startListing(std::move(context));
        return;
    }
    // One prefix exhausted: fall through to the next before deciding.
    if (context.prefixIndex + 1 < context.prefixes.size()) {
        advancePrefix(std::move(context));
        return;
    }
    // All prefixes listed. Prefer PVOL multi-tilt objects; fall back to SCAN
    // single-elevation assembly, then to TH-only objects.
    QVector<QPair<QString, QDateTime>> pvol;
    QVector<QPair<QString, QDateTime>> scan;
    for (const auto& candidate : context.candidates) {
        if (candidate.first.contains(QStringLiteral("/SCAN/")))
            scan.push_back(candidate);
        else
            pvol.push_back(candidate);
    }
    if (!pvol.isEmpty()) {
        std::sort(pvol.begin(), pvol.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        QVector<QPair<QString, QDateTime>> fresh;
        auto& seen = downloadedVolumeIds_[context.radarId];
        for (const auto& candidate : pvol) {
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
        return;
    }
    // TH-only PVOL fallback (a site temporarily serving unfiltered moments).
    QVector<QPair<QString, QDateTime>> pvolTh;
    for (const auto& candidate : context.thCandidates) {
        if (!candidate.first.contains(QStringLiteral("/SCAN/"))) pvolTh.push_back(candidate);
    }
    if (!pvolTh.isEmpty() && scan.isEmpty()) {
        std::sort(pvolTh.begin(), pvolTh.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        QVector<QPair<QString, QDateTime>> fresh;
        auto& seen = downloadedVolumeIds_[context.radarId];
        for (const auto& candidate : pvolTh) {
            const auto volumeId = candidate.second.toString(QStringLiteral("yyyyMMddTHHmm"));
            if (seen.contains(volumeId)) continue;
            if (!fresh.contains(candidate)) fresh.push_back(candidate);
            if (fresh.size() >= static_cast<std::size_t>(context.requestedCount)) break;
        }
        if (!fresh.isEmpty()) {
            beginDownloads(context.radarId, std::move(fresh), context.requestedCount);
            return;
        }
    }
    if (!scan.isEmpty()) {
        std::sort(scan.begin(), scan.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        // Skip buckets already delivered this session.
        auto& seen = downloadedVolumeIds_[context.radarId];
        QVector<QPair<QString, QDateTime>> unseen;
        for (const auto& candidate : scan) {
            const QDateTime bucketTime(candidate.second.date(),
                                       QTime(candidate.second.time().hour(),
                                             (candidate.second.time().minute() / 5) * 5, 0),
                                       QTimeZone::UTC);
            const auto bucketId = bucketTime.toString(QStringLiteral("yyyyMMddTHHmm"));
            if (seen.contains(bucketId)) continue;
            unseen.push_back(candidate);
        }
        if (unseen.isEmpty()) {
            inFlightRadars_.remove(context.radarId);
            emit statusChanged(context.radarId, QStringLiteral("OPERA %1 up to date")
                                                         .arg(context.radarId));
            return;
        }
        const auto groups = trimScanGroups(groupScanCandidates(unseen, context.requestedCount + 1),
                                             context.requestedCount,
                                             QDateTime::currentDateTimeUtc());
        if (groups.isEmpty()) {
            inFlightRadars_.remove(context.radarId);
            emit statusChanged(context.radarId, QStringLiteral("OPERA %1 up to date")
                                                         .arg(context.radarId));
            return;
        }
        QVector<VolumePlan> plans;
        for (const auto& group : groups) {
            QDateTime newest = group.front().second;
            for (const auto& file : group) newest = std::max(newest, file.second);
            const QDateTime bucketTime(newest.date(),
                                       QTime(newest.time().hour(),
                                             (newest.time().minute() / 5) * 5, 0),
                                       QTimeZone::UTC);
            plans.push_back({bucketTime.toString(QStringLiteral("yyyyMMddTHHmm")),
                             newest, group});
        }
        beginScanVolumes(context.radarId, std::move(plans));
        return;
    }
    // Last resort: SCAN TH-only files (no DBZH anywhere for the site).
    if (!context.thCandidates.isEmpty()) {
        QVector<QPair<QString, QDateTime>> scanTh;
        for (const auto& candidate : context.thCandidates) {
            if (candidate.first.contains(QStringLiteral("/SCAN/"))) scanTh.push_back(candidate);
        }
        if (!scanTh.isEmpty()) {
            std::sort(scanTh.begin(), scanTh.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            const auto groups = trimScanGroups(groupScanCandidates(scanTh, context.requestedCount + 1),
                                               context.requestedCount,
                                               QDateTime::currentDateTimeUtc());
            if (!groups.isEmpty()) {
                QVector<VolumePlan> plans;
                for (const auto& group : groups) {
                    QDateTime newest = group.front().second;
                    for (const auto& file : group) newest = std::max(newest, file.second);
                    const QDateTime bucketTime(
                        newest.date(),
                        QTime(newest.time().hour(), (newest.time().minute() / 5) * 5, 0),
                        QTimeZone::UTC);
                    plans.push_back({bucketTime.toString(QStringLiteral("yyyyMMddTHHmm")),
                                     newest, group});
                }
                beginScanVolumes(context.radarId, std::move(plans));
                return;
            }
        }
    }
    inFlightRadars_.remove(context.radarId);
    emit statusChanged(context.radarId, QStringLiteral("OPERA %1: no DBZH volumes found")
                                                 .arg(context.radarId));
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

void OperaSource::beginScanVolumes(const QString& radarId, QVector<VolumePlan> plans) {
    if (plans.isEmpty()) {
        inFlightRadars_.remove(radarId);
        emit statusChanged(radarId, QStringLiteral("OPERA %1 up to date").arg(radarId));
        return;
    }
    pendingScanVolumes_[radarId].append(plans);
    startNextScanVolume(radarId);
}

void OperaSource::startNextScanVolume(const QString& radarId) {
    if (cancelledRadars_.contains(radarId)) return;
    if (activeScanGroups_.contains(radarId)) return;
    auto it = pendingScanVolumes_.find(radarId);
    if (it == pendingScanVolumes_.end() || it->isEmpty()) {
        // No more queued volumes: release the radar once in-flight parts drain.
        for (const auto& pending : downloadRequests_) {
            if (pending.radarId == radarId) return;
        }
        inFlightRadars_.remove(radarId);
        return;
    }
    const VolumePlan plan = it->takeFirst();
    if (it->isEmpty()) pendingScanVolumes_.erase(it);
    downloadedVolumeIds_[radarId].insert(plan.volumeId);
    ScanGroupState state;
    state.dataTime = plan.dataTime;
    state.expected = plan.files.size();
    state.receivedAt = QDateTime::currentDateTimeUtc();
    activeScanGroups_.insert(radarId, std::move(state));
    emit volumeStarted(radarId, plan.volumeId, 1);
    emit statusChanged(radarId, QStringLiteral("OPERA downloading %1 %2 (%3 scans)")
                                         .arg(radarId, plan.volumeId)
                                         .arg(plan.files.size()));
    for (const auto& file : plan.files) {
        DownloadContext download;
        download.radarId = radarId;
        download.volumeId = plan.volumeId;
        download.key = file.first;
        download.dataTime = file.second;
        download.attempt = 0;
        download.scanPart = true;
        requestObject(std::move(download));
    }
}

void OperaSource::requestObject(DownloadContext context) {
    emit statusChanged(context.radarId, QStringLiteral("OPERA downloading %1 %2")
                                                 .arg(context.radarId, context.volumeId));
    auto* reply = network_.get(
        makeRequest(objectUrl(context.key), {}, QString::fromLatin1(kUserAgentSuffix),
                    CacheMode::PreferCache));
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
        if (context.scanPart) {
            completeScanPart(context, QDateTime::currentDateTimeUtc(), std::nullopt);
            return;
        }
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
                if (context.scanPart) {
                    guarded->completeScanPart(context, QDateTime::currentDateTimeUtc(),
                                              std::move(decoded));
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
        auto shared = std::make_shared<DecodedChunk>(std::move(*decoded));
        emit chunkDecoded(context.radarId, context.volumeId, context.key, std::move(shared),
                          receivedAt);
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

void OperaSource::completeScanPart(DownloadContext context, const QDateTime& receivedAt,
                                     std::optional<DecodedChunk> decoded) {
    auto groupIt = activeScanGroups_.find(context.radarId);
    if (groupIt == activeScanGroups_.end()) return;
    // Stale part from a previous group with a reused volume id: ignore.
    ScanGroupState& state = groupIt.value();
    if (decoded.has_value()) {
        state.parts.push_back(std::move(*decoded));
    } else {
        ++state.failed;
    }
    ++state.finished;
    state.receivedAt = receivedAt;
    if (state.finished < state.expected) {
        emit volumeProgress(context.radarId, context.volumeId, state.finished, state.expected,
                            static_cast<int>(state.parts.size()), state.failed);
        return;
    }
    // Move everything out before erasing: state dangles after this point.
    auto parts = std::move(state.parts);
    const int failed = state.failed;
    const int expected = state.expected;
    activeScanGroups_.erase(groupIt);
    if (!parts.empty()) {
        auto merged = mergeScanParts(std::move(parts), context.radarId.toStdString());
        auto shared = std::make_shared<DecodedChunk>(std::move(merged));
        emit chunkDecoded(context.radarId, context.volumeId, context.volumeId, std::move(shared),
                          receivedAt);
        emit volumeProgress(context.radarId, context.volumeId, 1, 1, 1, 0);
        emit volumeReady(context.radarId, context.volumeId, 1, 0, receivedAt);
        if (failed > 0) {
            emit statusChanged(context.radarId,
                               QStringLiteral("OPERA %1 %2: %3 of %4 scans unavailable")
                                   .arg(context.radarId, context.volumeId)
                                   .arg(failed)
                                   .arg(expected));
        }
    } else {
        emit volumeProgress(context.radarId, context.volumeId, 1, 1, 0, 1);
        emit volumeReady(context.radarId, context.volumeId, 1, 1, receivedAt);
        emit statusChanged(context.radarId, QStringLiteral("OPERA %1 %2: all %3 scans failed")
                                                     .arg(context.radarId, context.volumeId)
                                                     .arg(expected));
    }
    // Next queued volume, if any; otherwise release the radar when drained.
    startNextScanVolume(context.radarId);
}

void OperaSource::failRadar(const QString& radarId, const QString& message) {
    inFlightRadars_.remove(radarId);
    emit requestFailed(radarId, message);
}

} // namespace cappedweather
