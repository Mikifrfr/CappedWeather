#include "app/nexrad_source.hpp"

#include "core/radar_catalog.hpp"

#include <QDateTime>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>
#include <utility>

#ifdef CAPPEDWEATHER_HAS_BZIP2
#include <bzlib.h>
#endif

namespace cappedweather {

namespace {
constexpr auto kRealtimeBucket = "https://unidata-nexrad-level2-chunks.s3.amazonaws.com/";
constexpr auto kRadarCatalogUrl = "https://api.weather.gov/radar/stations";
constexpr int kVolumeQuietSeconds = 45;

int networkTimeoutMilliseconds() {
    return std::clamp(QSettings().value(QStringLiteral("network/timeoutSeconds"), 30).toInt(), 5, 120) * 1000;
}

int maximumConcurrentObjects() {
    return std::clamp(QSettings().value(QStringLiteral("network/parallelChunks"), 6).toInt(), 1, 12);
}

int maximumAttempts() {
    return std::clamp(QSettings().value(QStringLiteral("network/retries"), 2).toInt(), 0, 5) + 1;
}

int retryBackoffMilliseconds() {
    return std::clamp(QSettings().value(QStringLiteral("network/retryBackoffMs"), 250).toInt(), 100, 5000);
}

QString userAgent() {
    const auto contact = QSettings().value(QStringLiteral("network/contact")).toString().trimmed();
    return contact.isEmpty()
               ? QStringLiteral("CappedWeather/0.3 operational radar viewer")
               : QStringLiteral("CappedWeather/0.3 operational radar viewer (%1)").arg(contact);
}

void configureRequest(QNetworkRequest& request) {
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setTransferTimeout(networkTimeoutMilliseconds());
}

#ifdef CAPPEDWEATHER_HAS_BZIP2
std::optional<QByteArray> decompressChunk(const QByteArray& input, QString& error) {
    if (input.size() <= 6 || input[4] != 'B' || input[5] != 'Z' || input[6] != 'h') {
        error = QStringLiteral("NOAA object is not a bzip2 Level II chunk");
        return std::nullopt;
    }

    QByteArray compressed = input.mid(4);
    int capacity = 1 << 20;
    if (compressed.size() > capacity / 8) {
        capacity = static_cast<int>(compressed.size() * 8);
    }
    for (int attempt = 0; attempt < 7; ++attempt) {
        QByteArray output(capacity, Qt::Uninitialized);
        unsigned int outputLength = static_cast<unsigned int>(output.size());
        unsigned int inputLength = static_cast<unsigned int>(compressed.size());
        const int result = BZ2_bzBuffToBuffDecompress(output.data(), &outputLength,
                                                       compressed.data(), inputLength, 0, 0);
        if (result == BZ_OK) {
            output.resize(static_cast<qsizetype>(outputLength));
            return output;
        }
        if (result != BZ_OUTBUFF_FULL) {
            error = QStringLiteral("bzip2 decompression failed (%1)").arg(result);
            return std::nullopt;
        }
        capacity *= 2;
    }
    error = QStringLiteral("bzip2 output exceeded the safety limit");
    return std::nullopt;
}
#endif

QUrl listingUrl(const QString& prefix, const QString& delimiter, const QString& startAfter) {
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("list-type"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("prefix"), prefix);
    query.addQueryItem(QStringLiteral("max-keys"), QStringLiteral("1000"));
    if (!delimiter.isEmpty()) {
        query.addQueryItem(QStringLiteral("delimiter"), delimiter);
    }
    if (!startAfter.isEmpty()) {
        query.addQueryItem(QStringLiteral("start-after"), startAfter);
    }
    return QUrl(QStringLiteral("https://unidata-nexrad-level2-chunks.s3.amazonaws.com/?") +
                query.toString(QUrl::FullyEncoded));
}

QStringList newestFirstVolumePrefixes(const QStringList& prefixes) {
    struct NumberedPrefix { int number; QString prefix; };
    QVector<NumberedPrefix> numbered;
    QStringList other;
    for (const auto& prefix : prefixes) {
        const auto parts = prefix.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        bool ok = false;
        const auto number = parts.isEmpty() ? -1 : parts.constLast().toInt(&ok);
        if (ok && number >= 0 && number <= 999) {
            numbered.push_back({number, prefix});
        } else {
            other.push_back(prefix);
        }
    }
    if (numbered.isEmpty()) {
        std::sort(other.begin(), other.end(), std::greater<>());
        return other;
    }

    std::sort(numbered.begin(), numbered.end(), [](const auto& left, const auto& right) {
        return left.number < right.number;
    });
    int newest = numbered.constLast().number;
    int largestGap = -1;
    for (qsizetype index = 0; index < numbered.size(); ++index) {
        const auto current = numbered.at(index).number;
        const auto next = index + 1 < numbered.size() ? numbered.at(index + 1).number
                                                       : numbered.constFirst().number + 1000;
        const auto gap = next - current - 1;
        if (gap > largestGap) {
            largestGap = gap;
            newest = current;
        }
    }
    std::sort(numbered.begin(), numbered.end(), [newest](const auto& left, const auto& right) {
        const auto leftAge = (newest - left.number + 1000) % 1000;
        const auto rightAge = (newest - right.number + 1000) % 1000;
        return leftAge < rightAge;
    });
    QStringList result;
    result.reserve(numbered.size() + other.size());
    for (const auto& item : numbered) result.push_back(item.prefix);
    std::sort(other.begin(), other.end(), std::greater<>());
    result.append(other);
    return result;
}

}

NexradSource::NexradSource(QObject* parent)
    : QObject(parent) {
    connect(&network_, &QNetworkAccessManager::finished, this, [this](QNetworkReply* reply) {
        if (catalogRequests_.contains(reply)) {
            catalogFinished(reply);
        } else if (rootListingRequests_.contains(reply)) {
            rootListingFinished(reply);
        } else if (volumeListingRequests_.contains(reply)) {
            volumeListingFinished(reply);
        } else if (objectRequests_.contains(reply)) {
            objectFinished(reply);
        }
    });
}

void NexradSource::requestCatalog() {
    QNetworkRequest request(QUrl(QString::fromUtf8(kRadarCatalogUrl)));
    configureRequest(request);
    auto* reply = network_.get(request);
    catalogRequests_.insert(reply, true);
}

void NexradSource::catalogFinished(QNetworkReply* reply) {
    catalogRequests_.remove(reply);
    if (reply->error() != QNetworkReply::NoError) {
        const auto error = reply->errorString();
        reply->deleteLater();
        emit requestFailed(QStringLiteral("CATALOG"), error);
        return;
    }
    const auto data = reply->readAll();
    reply->deleteLater();
    emit catalogReceived(data);
}

void NexradSource::requestLatest(const QString& radarId) {
    requestRecent(radarId, 1);
}

void NexradSource::requestRecent(const QString& radarId, const int volumeCount) {
    if (!RadarCatalog::isValidRadarId(radarId.toStdString())) {
        emit requestFailed(radarId, QStringLiteral("Invalid radar identifier"));
        return;
    }
    if (activeRadars_.contains(radarId)) {
        return;
    }

    cancelledRadars_.remove(radarId);
    activeRadars_.insert(radarId);
    startRootListing(radarId, {}, {}, std::clamp(volumeCount, 1, 12));
}

void NexradSource::clearDownloaded(const QString& radarId) {
    downloadedVolumeIds_.remove(radarId);
}

void NexradSource::cancel(const QString& radarId) {
    cancelledRadars_.insert(radarId);
    const auto rootReplies = rootListingRequests_.keys();
    for (auto* reply : rootReplies) {
        if (rootListingRequests_.value(reply).radarId != radarId) continue;
        rootListingRequests_.remove(reply);
        reply->abort();
        reply->deleteLater();
    }
    const auto volumeReplies = volumeListingRequests_.keys();
    for (auto* reply : volumeReplies) {
        if (volumeListingRequests_.value(reply).radarId != radarId) continue;
        volumeListingRequests_.remove(reply);
        reply->abort();
        reply->deleteLater();
    }
    const auto objectReplies = objectRequests_.keys();
    for (auto* reply : objectReplies) {
        if (objectRequests_.value(reply).object.radarId != radarId) continue;
        objectRequests_.remove(reply);
        activeObjectRequests_ = std::max(0, activeObjectRequests_ - 1);
        reply->abort();
        reply->deleteLater();
    }
    for (qsizetype index = queuedObjects_.size() - 1; index >= 0; --index) {
        if (queuedObjects_.at(index).object.radarId == radarId) queuedObjects_.removeAt(index);
    }
    pumpObjectQueue();
    finishRadar(radarId);
    emit statusChanged(radarId, QStringLiteral("cancelled"));
}

void NexradSource::startRootListing(const QString& radarId, const QString& startAfter,
                                    QStringList volumePrefixes, const int requestedCount) {
    QNetworkRequest request(listingUrl(utcPrefix(radarId), QStringLiteral("/"), startAfter));
    configureRequest(request);
    auto* reply = network_.get(request);
    rootListingRequests_.insert(
        reply, ListingContext{radarId, startAfter, std::move(volumePrefixes), requestedCount});
    emit statusChanged(radarId, QStringLiteral("checking for new volume"));
}

void NexradSource::rootListingFinished(QNetworkReply* reply) {
    const auto context = rootListingRequests_.take(reply);
    if (reply->error() != QNetworkReply::NoError) {
        const auto error = reply->errorString();
        const auto radarId = context.radarId;
        reply->deleteLater();
        finishRadar(radarId);
        emit requestFailed(radarId, error);
        return;
    }

    QXmlStreamReader xml(reply->readAll());
    auto prefixes = context.volumePrefixes;
    bool truncated = false;
    QString lastPrefix;
    while (!xml.atEnd()) {
        xml.readNextStartElement();
        if (xml.name() == QStringLiteral("IsTruncated")) {
            truncated = xml.readElementText() == QStringLiteral("true");
        } else if (xml.name() == QStringLiteral("CommonPrefixes")) {
            while (xml.readNextStartElement()) {
                if (xml.name() == QStringLiteral("Prefix")) {
                    lastPrefix = xml.readElementText();
                    if (!lastPrefix.isEmpty()) {
                        prefixes.push_back(lastPrefix);
                    }
                } else {
                    xml.skipCurrentElement();
                }
            }
        }
    }
    const auto xmlError = xml.errorString();
    reply->deleteLater();

    if (xml.hasError()) {
        finishRadar(context.radarId);
        emit requestFailed(context.radarId, QStringLiteral("Invalid NOAA volume listing: %1").arg(xmlError));
        return;
    }
    if (truncated && !lastPrefix.isEmpty() && lastPrefix != context.startAfter) {
        startRootListing(context.radarId, lastPrefix, std::move(prefixes), context.requestedCount);
        return;
    }
    if (prefixes.isEmpty()) {
        finishRadar(context.radarId);
        emit statusChanged(context.radarId, QStringLiteral("no data"));
        return;
    }

    startVolumeListing(context.radarId, newestFirstVolumePrefixes(prefixes), 0,
                       context.requestedCount, 0);
}

void NexradSource::startVolumeListing(const QString& radarId, const QStringList& volumePrefixes,
                                      const int candidateIndex, const int requestedCount,
                                      const int completedCount) {
    if (candidateIndex < 0 || candidateIndex >= volumePrefixes.size()) {
        finishRadar(radarId);
        emit statusChanged(radarId, completedCount > 0
                                        ? QStringLiteral("history loaded")
                                        : downloadedVolumeIds_.value(radarId).isEmpty()
                                              ? QStringLiteral("no complete volume")
                                              : QStringLiteral("up to date"));
        return;
    }
    requestVolumePage(VolumeListingContext{
        radarId, volumePrefixes.at(candidateIndex), {}, volumePrefixes, candidateIndex,
        requestedCount, completedCount, {},
    });
}

void NexradSource::requestVolumePage(VolumeListingContext context) {
    QNetworkRequest request(listingUrl(context.volumePrefix, {}, context.startAfter));
    configureRequest(request);
    auto* reply = network_.get(request);
    volumeListingRequests_.insert(reply, std::move(context));
    emit statusChanged(volumeListingRequests_.value(reply).radarId, QStringLiteral("checking volume chunks"));
}

void NexradSource::volumeListingFinished(QNetworkReply* reply) {
    auto context = volumeListingRequests_.take(reply);
    if (reply->error() != QNetworkReply::NoError) {
        const auto error = reply->errorString();
        const auto radarId = context.radarId;
        reply->deleteLater();
        finishRadar(radarId);
        emit requestFailed(radarId, error);
        return;
    }

    QXmlStreamReader xml(reply->readAll());
    bool truncated = false;
    QString lastKey;
    while (!xml.atEnd()) {
        xml.readNextStartElement();
        if (xml.name() == QStringLiteral("IsTruncated")) {
            truncated = xml.readElementText() == QStringLiteral("true");
            continue;
        }
        if (xml.name() != QStringLiteral("Contents")) {
            continue;
        }

        QString key;
        QString modified;
        quint64 size = 0;
        while (xml.readNextStartElement()) {
            if (xml.name() == QStringLiteral("Key")) {
                key = xml.readElementText();
            } else if (xml.name() == QStringLiteral("LastModified")) {
                modified = xml.readElementText();
            } else if (xml.name() == QStringLiteral("Size")) {
                size = xml.readElementText().toULongLong();
            } else {
                xml.skipCurrentElement();
            }
        }
        if (!key.isEmpty()) {
            lastKey = key;
        }
        if (key.endsWith(QStringLiteral("-I")) && size > 0) {
            context.objects.push_back({context.radarId, key, modified, size});
        }
    }
    const auto xmlError = xml.errorString();
    reply->deleteLater();

    if (xml.hasError()) {
        finishRadar(context.radarId);
        emit requestFailed(context.radarId, QStringLiteral("Invalid NOAA chunk listing: %1").arg(xmlError));
        return;
    }
    if (truncated && !lastKey.isEmpty() && lastKey != context.startAfter) {
        context.startAfter = lastKey;
        requestVolumePage(std::move(context));
        return;
    }
    if (context.objects.isEmpty()) {
        startVolumeListing(context.radarId, context.volumePrefixes, context.candidateIndex + 1,
                           context.requestedCount, context.completedCount);
        return;
    }

    const auto latest = *std::max_element(context.objects.cbegin(), context.objects.cend(),
                                          [](const PendingObject& left, const PendingObject& right) {
                                              return left.lastModified < right.lastModified ||
                                                     (left.lastModified == right.lastModified && left.key < right.key);
                                          });
    const auto latestTime = QDateTime::fromString(latest.lastModified, Qt::ISODate);
    const auto ageSeconds = latestTime.isValid()
                                ? latestTime.secsTo(QDateTime::currentDateTimeUtc())
                                : kVolumeQuietSeconds;
    if (context.candidateIndex == 0 && ageSeconds < kVolumeQuietSeconds &&
        context.volumePrefixes.size() > 1) {
        startVolumeListing(context.radarId, context.volumePrefixes, 1,
                           context.requestedCount, context.completedCount);
        return;
    }

    if (downloadedVolumeIds_.value(context.radarId).contains(context.volumePrefix)) {
        startVolumeListing(context.radarId, context.volumePrefixes, context.candidateIndex + 1,
                           context.requestedCount, context.completedCount);
        return;
    }
    beginVolume(context.radarId, context.volumePrefix, std::move(context.objects));
    auto& active = activeVolumes_[context.radarId];
    active.volumePrefixes = context.volumePrefixes;
    active.candidateIndex = context.candidateIndex;
    active.requestedCount = context.requestedCount;
    active.completedCount = context.completedCount;
}

void NexradSource::beginVolume(const QString& radarId, const QString& volumeId,
                               QVector<PendingObject> objects) {
    if (objects.isEmpty()) {
        finishRadar(radarId);
        emit statusChanged(radarId, QStringLiteral("no image chunks"));
        return;
    }

    const auto chunkCount = static_cast<int>(objects.size());
    QDateTime dataTime;
    for (const auto& object : objects) {
        const auto modified = QDateTime::fromString(object.lastModified, Qt::ISODate);
        if (modified.isValid() && (!dataTime.isValid() || modified > dataTime)) dataTime = modified;
    }
    activeVolumes_.insert(radarId, ActiveVolume{radarId, volumeId, chunkCount, 0, 0, {}, dataTime});
    emit volumeStarted(radarId, volumeId, chunkCount);
    emit volumeProgress(radarId, volumeId, 0, chunkCount, 0, 0);
    emit statusChanged(radarId, QStringLiteral("downloading %1 volume chunks").arg(objects.size()));
    for (const auto& object : objects) {
        requestObject(ObjectContext{object, volumeId, 0});
    }
}

void NexradSource::requestObject(ObjectContext context) {
    if (cancelledRadars_.contains(context.object.radarId)) return;
    if (activeObjectRequests_ >= maximumConcurrentObjects()) {
        queuedObjects_.enqueue(std::move(context));
        return;
    }
    QNetworkRequest request(objectUrl(context.object.key));
    configureRequest(request);
    auto* reply = network_.get(request);
    objectRequests_.insert(reply, std::move(context));
    ++activeObjectRequests_;
}

void NexradSource::pumpObjectQueue() {
    while (activeObjectRequests_ < maximumConcurrentObjects() && !queuedObjects_.isEmpty()) {
        auto context = queuedObjects_.dequeue();
        if (!cancelledRadars_.contains(context.object.radarId)) requestObject(std::move(context));
    }
}

void NexradSource::objectFinished(QNetworkReply* reply) {
    auto context = objectRequests_.take(reply);
    activeObjectRequests_ = std::max(0, activeObjectRequests_ - 1);
    const auto radarId = context.object.radarId;
    auto active = activeVolumes_.find(radarId);
    if (active == activeVolumes_.end()) {
        reply->deleteLater();
        return;
    }

    const auto receivedAt = QDateTime::currentDateTimeUtc();
    QString failure;
    bool decoded = false;
    if (reply->error() != QNetworkReply::NoError) {
        failure = reply->errorString();
    } else {
        const auto data = reply->readAll();
#ifdef CAPPEDWEATHER_HAS_BZIP2
        QString decompressionError;
        if (const auto decompressed = decompressChunk(data, decompressionError); decompressed.has_value()) {
            decoded = true;
            ++active->decodedChunks;
            emit chunkDecompressed(radarId, context.volumeId, context.object.key, *decompressed, receivedAt);
        } else {
            failure = decompressionError;
        }
#else
        Q_UNUSED(data);
        failure = QStringLiteral("bzip2 support unavailable");
#endif
    }
    reply->deleteLater();

    const auto attempts = maximumAttempts();
    if (!decoded && context.attempt + 1 < attempts && !cancelledRadars_.contains(radarId)) {
        ++context.attempt;
        emit statusChanged(radarId, QStringLiteral("retrying volume chunk (%1/%2)")
                                        .arg(context.attempt + 1).arg(attempts));
        const auto delayMilliseconds = retryBackoffMilliseconds() * (1 << context.attempt);
        QTimer::singleShot(delayMilliseconds, this, [this, context = std::move(context)]() mutable {
            requestObject(std::move(context));
            pumpObjectQueue();
        });
        pumpObjectQueue();
        return;
    }
    if (!decoded) {
        ++active->failedChunks;
        if (active->firstError.isEmpty()) active->firstError = failure;
    }
    --active->remaining;
    const auto total = active->decodedChunks + active->failedChunks + active->remaining;
    emit volumeProgress(radarId, active->volumeId, total - active->remaining, total,
                        active->decodedChunks, active->failedChunks);

    if (active->remaining != 0) {
        pumpObjectQueue();
        return;
    }

    const auto volumeId = active->volumeId;
    const auto decodedChunks = active->decodedChunks;
    const auto failedChunks = active->failedChunks;
    const auto firstError = active->firstError;
    const auto dataTime = active->dataTime;
    const auto volumePrefixes = active->volumePrefixes;
    const auto nextIndex = active->candidateIndex + 1;
    const auto requestedCount = active->requestedCount;
    const auto completedCount = active->completedCount + 1;
    if (decodedChunks == 0) {
        finishRadar(radarId);
        const auto detail = firstError.isEmpty()
                                ? QStringLiteral("unknown network or decode error")
                                : firstError;
        emit requestFailed(radarId, QStringLiteral("%1 of %2 volume chunks failed: %3")
                                      .arg(failedChunks)
                                      .arg(failedChunks + decodedChunks)
                                      .arg(detail));
        pumpObjectQueue();
        return;
    }

    downloadedVolumeIds_[radarId].insert(volumeId);
    emit volumeReady(radarId, volumeId, decodedChunks + failedChunks, failedChunks,
                     dataTime.isValid() ? dataTime : receivedAt);
    activeVolumes_.remove(radarId);
    if (completedCount < requestedCount) {
        emit statusChanged(radarId, QStringLiteral("loading history %1 of %2")
                                       .arg(completedCount).arg(requestedCount));
        startVolumeListing(radarId, volumePrefixes, nextIndex, requestedCount, completedCount);
    } else {
        activeRadars_.remove(radarId);
        emit statusChanged(radarId, failedChunks == 0
                                        ? QStringLiteral("received %1 complete volume%2")
                                              .arg(completedCount)
                                              .arg(completedCount == 1 ? QString{} : QStringLiteral("s"))
                                        : QStringLiteral("received partial volume (%1 chunk%2 unavailable)")
                                              .arg(failedChunks)
                                              .arg(failedChunks == 1 ? QString{} : QStringLiteral("s")));
    }
    pumpObjectQueue();
}

void NexradSource::finishRadar(const QString& radarId) {
    activeRadars_.remove(radarId);
    activeVolumes_.remove(radarId);
}

QString NexradSource::utcPrefix(const QString& radarId) {
    return radarId + QLatin1Char('/');
}

QUrl NexradSource::objectUrl(const QString& key) {
    QUrl url(QString::fromUtf8(kRealtimeBucket));
    url.setPath(url.path() + key);
    return url;
}

} // namespace cappedweather
