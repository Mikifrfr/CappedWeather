#include "app/archive_source.hpp"
#include "app/http_client.hpp"

#include "core/radar_catalog.hpp"

#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QNetworkReply>
#include <QPointer>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <algorithm>

namespace cappedweather {
namespace {

constexpr qint64 kMaximumArchiveBytes = 512LL * 1024 * 1024;
const QString kArchiveUserAgentSuffix = QStringLiteral(" NOAA public Level-II archive client");

int archiveParallelDownloads() {
    return std::clamp(QSettings().value(QStringLiteral("archive/parallelDownloads"), 4).toInt(), 1, 12);
}

} // namespace

ArchiveSource::ArchiveSource(QObject* parent)
    : QObject(parent) {
    qRegisterMetaType<std::shared_ptr<cappedweather::DecodedChunk>>();
    processingPool_.setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 2, 8));
    connect(&network_, &QNetworkAccessManager::finished, this, [this](QNetworkReply* reply) {
        if (listingRequests_.contains(reply)) listingFinished(reply);
        else if (objectRequests_.contains(reply)) objectFinished(reply);
        else reply->deleteLater();
    });
}

ArchiveSource::~ArchiveSource() {
    disconnect(&network_, nullptr, this, nullptr);
    ++localGeneration_;
    for (const auto& job : jobs_) job->cancelled = true;
    for (auto it = listingRequests_.begin(); it != listingRequests_.end(); ++it) it.key()->abort();
    for (auto it = objectRequests_.begin(); it != objectRequests_.end(); ++it) it.key()->abort();
    processingPool_.waitForDone();
}

void ArchiveSource::loadFiles(const QString& requestedRadarId, const QStringList& paths) {
    const auto radarId = requestedRadarId.trimmed().toUpper();
    if (!RadarCatalog::isValidRadarId(radarId.toStdString()) || paths.isEmpty()) {
        emit requestFailed(radarId, QStringLiteral("Select a radar and one or more Archive-II files"));
        return;
    }
    emit statusChanged(radarId, QStringLiteral("decoding %1 local Archive-II file%2")
                                    .arg(paths.size()).arg(paths.size() == 1 ? QString{} : QStringLiteral("s")));
    const QPointer<ArchiveSource> guard(this);
    const auto generation = ++localGeneration_;
    for (const auto& path : paths) {
        processingPool_.start([guard, generation, radarId, path] {
            QString error;
            std::optional<ArchiveLevel2Result> decoded;
            QDateTime observedAt = archiveTimeFromFileName(path);
            QFileInfo info(path);
            if (!info.exists() || !info.isFile()) {
                error = QStringLiteral("file does not exist");
            } else if (info.size() > kMaximumArchiveBytes) {
                error = QStringLiteral("Archive-II file exceeds the 512 MB safety limit");
            } else {
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) error = file.errorString();
                else decoded = decodeLevel2Archive(file.readAll(), &error, radarId);
            }
            if (!observedAt.isValid()) observedAt = info.lastModified().toUTC();
            if (!guard) return;
            QMetaObject::invokeMethod(guard, [guard, generation, radarId, path, observedAt,
                                              decoded = std::move(decoded), error = std::move(error)]() mutable {
                if (!guard || generation != guard->localGeneration_) return;
                const auto volumeId = QFileInfo(path).fileName();
                if (!decoded) {
                    emit guard->requestFailed(radarId,
                        QStringLiteral("%1: %2").arg(volumeId, error));
                    return;
                }
                if (decoded->radarId != radarId) {
                    emit guard->requestFailed(radarId,
                        QStringLiteral("%1 belongs to %2, not selected radar %3")
                            .arg(volumeId, decoded->radarId, radarId));
                    return;
                }
                const auto recordCount = std::max(1, decoded->compressedRecords);
                const auto decodedRecords = std::max(0, recordCount - decoded->failedRecords);
                emit guard->volumeStarted(radarId, volumeId, recordCount);
                emit guard->volumeProgress(radarId, volumeId, recordCount, recordCount,
                                           decodedRecords, decoded->failedRecords);
                emit guard->chunkDecoded(radarId, volumeId, QFileInfo(path).absoluteFilePath(),
                                         std::make_shared<DecodedChunk>(std::move(decoded->decoded)),
                                         observedAt);
                emit guard->volumeReady(radarId, volumeId, recordCount,
                                        decoded->failedRecords, observedAt);
                emit guard->statusChanged(radarId,
                    QStringLiteral("loaded local Archive-II volume %1").arg(volumeId));
            }, Qt::QueuedConnection);
        });
    }
}

void ArchiveSource::requestAws(const QString& requestedRadarId, const QDateTime& start,
                               const QDateTime& end, const int maximumVolumes) {
    const auto radarId = requestedRadarId.trimmed().toUpper();
    const auto startUtc = start.toUTC();
    const auto endUtc = end.toUTC();
    if (!RadarCatalog::isValidRadarId(radarId.toStdString()) || !startUtc.isValid() ||
        !endUtc.isValid() || startUtc > endUtc) {
        emit requestFailed(radarId, QStringLiteral("Invalid radar or UTC archive time window"));
        return;
    }
    if (startUtc.daysTo(endUtc) > 31) {
        emit requestFailed(radarId, QStringLiteral("AWS archive window cannot exceed 31 days"));
        return;
    }
    auto job = std::make_shared<Job>();
    job->radarId = radarId;
    job->startUtc = startUtc;
    job->endUtc = endUtc;
    job->maximumVolumes = std::max(0, maximumVolumes);
    jobs_.push_back(job);
    for (auto date = startUtc.date(); date <= endUtc.date(); date = date.addDays(1)) {
        auto* reply = network_.get(
            makeRequest(awsArchiveListingUrl(radarId, date), {}, kArchiveUserAgentSuffix));
        listingRequests_.insert(reply, job);
        ++job->pendingListings;
    }
    emit statusChanged(radarId, QStringLiteral("querying public AWS Level-II archive"));
}

void ArchiveSource::cancel() {
    ++localGeneration_;
    for (const auto& job : jobs_) job->cancelled = true;
    for (auto it = listingRequests_.begin(); it != listingRequests_.end(); ++it) it.key()->abort();
    for (auto it = objectRequests_.begin(); it != objectRequests_.end(); ++it) it.key()->abort();
    jobs_.clear();
}

void ArchiveSource::listingFinished(QNetworkReply* reply) {
    const auto job = listingRequests_.take(reply);
    if (!job) {
        reply->deleteLater();
        return;
    }
    if (!job->cancelled) {
        if (reply->error() == QNetworkReply::NoError) {
            QString error;
            const auto objects = parseAwsArchiveListing(reply->readAll(), job->radarId, &error);
            job->listed.append(objects);
            if (!error.isEmpty()) emit requestFailed(job->radarId, error);
        } else if (reply->error() != QNetworkReply::OperationCanceledError) {
            emit requestFailed(job->radarId,
                               QStringLiteral("AWS archive listing: %1").arg(reply->errorString()));
        }
    }
    reply->deleteLater();
    --job->pendingListings;
    if (job->pendingListings == 0 && !job->cancelled) beginDownloads(job);
}

void ArchiveSource::beginDownloads(const std::shared_ptr<Job>& job) {
    auto objects = selectArchiveObjects(std::move(job->listed), job->startUtc, job->endUtc,
                                        job->maximumVolumes);
    if (objects.isEmpty()) {
        emit requestFailed(job->radarId, QStringLiteral("No public Level-II volumes matched the selected time window"));
        jobs_.removeOne(job);
        return;
    }
    for (auto& object : objects) job->queued.enqueue(std::move(object));
    emit statusChanged(job->radarId,
                       QStringLiteral("downloading %1 AWS archive volume%2")
                           .arg(job->queued.size()).arg(job->queued.size() == 1 ? QString{} : QStringLiteral("s")));
    pump(job);
}

void ArchiveSource::pump(const std::shared_ptr<Job>& job) {
    while (!job->cancelled && job->activeDownloads < archiveParallelDownloads() && !job->queued.isEmpty()) {
        const auto object = job->queued.dequeue();
        auto* reply =
            network_.get(makeRequest(awsArchiveObjectUrl(object.key), {}, kArchiveUserAgentSuffix));
        reply->setReadBufferSize(kMaximumArchiveBytes + 1);
        connect(reply, &QNetworkReply::readyRead, this, [reply] {
            if (reply->bytesAvailable() <= kMaximumArchiveBytes) return;
            reply->setProperty("cappedweatherTooLarge", true);
            reply->abort();
        });
        objectRequests_.insert(reply, {job, object});
        ++job->activeDownloads;
    }
}

void ArchiveSource::objectFinished(QNetworkReply* reply) {
    auto context = objectRequests_.take(reply);
    const auto job = context.job;
    if (job) --job->activeDownloads;
    if (!job || job->cancelled) {
        reply->deleteLater();
        return;
    }
    if (reply->property("cappedweatherTooLarge").toBool()) {
        reply->deleteLater();
        completeObject(job, context.object, std::nullopt,
                       QStringLiteral("download exceeded the 512 MB safety limit"));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const auto error = reply->errorString();
        reply->deleteLater();
        completeObject(job, context.object, std::nullopt, error);
        return;
    }
    auto bytes = reply->readAll();
    reply->deleteLater();
    if (bytes.size() > kMaximumArchiveBytes) {
        completeObject(job, context.object, std::nullopt,
                       QStringLiteral("download exceeded the 512 MB safety limit"));
        return;
    }
    const QPointer<ArchiveSource> guard(this);
    ++job->pendingDecodes;
    processingPool_.start([guard, job, object = context.object, bytes = std::move(bytes)]() mutable {
        QString error;
        auto decoded = decodeLevel2Archive(bytes, &error, job->radarId);
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, job, object = std::move(object),
                                          decoded = std::move(decoded), error = std::move(error)]() mutable {
            if (guard) guard->completeObject(job, object, std::move(decoded), std::move(error));
        }, Qt::QueuedConnection);
    });
}

void ArchiveSource::completeObject(const std::shared_ptr<Job>& job, const AwsArchiveObject& object,
                                   std::optional<ArchiveLevel2Result> decoded, QString error) {
    if (!job || job->cancelled) return;
    job->pendingDecodes = std::max(0, job->pendingDecodes - 1);
    ++job->completed;
    if (decoded && decoded->radarId == job->radarId) {
        ++job->decoded;
        const auto recordCount = std::max(1, decoded->compressedRecords);
        const auto decodedRecords = std::max(0, recordCount - decoded->failedRecords);
        // MainWindow assembles one pending volume per radar. Publish each fully
        // decoded archive atomically even though network/decode work is parallel.
        emit volumeStarted(job->radarId, object.key, recordCount);
        emit volumeProgress(job->radarId, object.key, recordCount, recordCount,
                            decodedRecords, decoded->failedRecords);
        emit chunkDecoded(job->radarId, object.key, object.key,
                          std::make_shared<DecodedChunk>(std::move(decoded->decoded)),
                          object.observedAt);
        emit volumeReady(job->radarId, object.key, recordCount,
                         decoded->failedRecords, object.observedAt);
    } else {
        ++job->failed;
        if (error.isEmpty()) error = QStringLiteral("archive radar identifier did not match request");
        emit volumeProgress(job->radarId, object.key, 1, 1, 0, 1);
        emit requestFailed(job->radarId,
                           QStringLiteral("%1: %2").arg(QFileInfo(object.key).fileName(), error));
    }
    pump(job);
    finishIfDone(job);
}

void ArchiveSource::finishIfDone(const std::shared_ptr<Job>& job) {
    if (job->activeDownloads > 0 || job->pendingDecodes > 0 || !job->queued.isEmpty()) return;
    emit statusChanged(job->radarId,
        QStringLiteral("AWS archive loaded: %1 decoded, %2 failed")
            .arg(job->decoded).arg(job->failed));
    jobs_.removeOne(job);
}

} // namespace cappedweather
