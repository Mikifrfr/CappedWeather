#include "app/glm_source.hpp"

#include "core/glm_l2_reader.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSettings>
#include <QTemporaryFile>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cappedweather {
namespace {

void configureRequest(QNetworkRequest& request) {
    const auto contact = QSettings().value(QStringLiteral("network/contact")).toString().trimmed();
    const auto agent = contact.isEmpty()
        ? QStringLiteral("CappedWeather/0.3 GLM live client")
        : QStringLiteral("CappedWeather/0.3 GLM live client (%1)").arg(contact);
    request.setRawHeader("User-Agent", agent.toUtf8());
    request.setTransferTimeout(std::clamp(
        QSettings().value(QStringLiteral("network/timeoutSeconds"), 30).toInt(), 5, 120) * 1000);
}

std::int64_t cellKey(const int cellX, const int cellY) {
    return (static_cast<std::int64_t>(cellX) << 32) ^
           (static_cast<std::int64_t>(cellY) & 0xFFFFFFFFLL);
}

} // namespace

GlmSource::GlmSource(QObject* parent)
    : QObject(parent), grid_(config_.grid) {
    qRegisterMetaType<QVector<LightningCellSnapshot>>();
    // The netCDF/HDF5 runtime distributed with common Windows builds is not
    // thread-safe. Keep parsing off the UI thread, but serialize file reads.
    processingPool_.setMaxThreadCount(1);
    timer_.setTimerType(Qt::CoarseTimer);
    connect(&timer_, &QTimer::timeout, this, &GlmSource::refresh);
    connect(&network_, &QNetworkAccessManager::finished, this, &GlmSource::requestFinished);
}

void GlmSource::start() {
    if (running_) return;
    running_ = true;
    timer_.start(std::clamp(config_.refreshSeconds, 10, 300) * 1000);
    refresh();
}

void GlmSource::stop() {
    running_ = false;
    timer_.stop();
    refreshPending_ = false;
    for (auto* reply : requests_.keys()) reply->abort();
}

void GlmSource::configure(GlmLiveConfig config) {
    config.refreshSeconds = std::clamp(config.refreshSeconds, 10, 300);
    config.initialBackfillMinutes = std::clamp(config.initialBackfillMinutes, 13, 60);
    config.maximumConcurrentDownloads = std::clamp(config.maximumConcurrentDownloads, 1, 8);
    config.cessationRetentionMinutes = std::clamp(config.cessationRetentionMinutes, 1, 60);
    const bool detectorChanged = config.grid.cellSizeDeg != config_.grid.cellSizeDeg ||
        config.grid.jump.rateResolutionSec != config_.grid.jump.rateResolutionSec ||
        config.grid.jump.sigmaWindowSec != config_.grid.jump.sigmaWindowSec ||
        config.grid.jump.jumpSigmaThreshold != config_.grid.jump.jumpSigmaThreshold ||
        config.grid.jump.minFlashRatePerMin != config_.grid.jump.minFlashRatePerMin ||
        config.grid.jump.minSigmaSamples != config_.grid.jump.minSigmaSamples;
    config_ = config;
    if (detectorChanged) {
        grid_ = FlashRateGrid(config_.grid);
        cells_.clear();
        seenKeys_.clear();
        seenKeyOrder_.clear();
        latestProcessedBySatellite_.clear();
        initialBackfillComplete_ = false;
    }
    if (running_) timer_.start(config_.refreshSeconds * 1000);
}

void GlmSource::refresh() {
    if (!running_) return;
    if (!glmL2ReaderAvailable()) {
        emit requestFailed(QStringLiteral("GLM live view unavailable: this build has no netCDF-C support"));
        emit statusChanged(QStringLiteral("GLM disabled (netCDF-C unavailable)"));
        timer_.stop();
        running_ = false;
        return;
    }
    if (batchActive_) {
        refreshPending_ = true;
        return;
    }

    batchActive_ = true;
    refreshPending_ = false;
    batchFailures_ = 0;
    batchFilesProcessed_ = 0;
    batchFlashesProcessed_ = 0;
    candidates_.clear();
    parsedFiles_.clear();
    const auto now = QDateTime::currentDateTimeUtc();
    const auto currentHour = QDateTime(now.date(), QTime(now.time().hour(), 0), Qt::UTC);
    const int hours = initialBackfillComplete_ ? 2 : 2;
    pendingListings_ = hours * 2;
    emit statusChanged(initialBackfillComplete_
        ? QStringLiteral("checking GOES-18/19 GLM")
        : QStringLiteral("backfilling live GOES-18/19 GLM history"));
    for (const auto& satellite : {QStringLiteral("G19"), QStringLiteral("G18")})
        for (int offset = 0; offset < hours; ++offset)
            requestListing(satellite, currentHour.addSecs(-offset * 3600));
}

QString GlmSource::prefixForHour(const QDateTime& hour) {
    return QString::fromStdString(glmLcfaPrefix(hour.date().year(), hour.date().dayOfYear(),
                                                hour.time().hour()));
}

QUrl GlmSource::listingUrl(const QString& satellite, const QString& prefix) {
    const auto bucket = satellite == QStringLiteral("G18")
        ? QStringLiteral("noaa-goes18") : QStringLiteral("noaa-goes19");
    QUrl url(QStringLiteral("https://%1.s3.amazonaws.com/").arg(bucket));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("list-type"), QStringLiteral("2"));
    query.addQueryItem(QStringLiteral("prefix"), prefix);
    url.setQuery(query);
    return url;
}

QUrl GlmSource::objectUrl(const QString& satellite, const QString& key) {
    const auto bucket = satellite == QStringLiteral("G18")
        ? QStringLiteral("noaa-goes18") : QStringLiteral("noaa-goes19");
    return QUrl(QStringLiteral("https://%1.s3.amazonaws.com/%2").arg(bucket, key));
}

void GlmSource::requestListing(const QString& satellite, const QDateTime& hour) {
    QNetworkRequest request(listingUrl(satellite, prefixForHour(hour)));
    configureRequest(request);
    auto* reply = network_.get(request);
    requests_.insert(reply, {RequestKind::Listing, satellite, {}, 0});
}

void GlmSource::requestObject(PendingObject object) {
    QNetworkRequest request(objectUrl(object.satellite, object.key));
    configureRequest(request);
    auto* reply = network_.get(request);
    requests_.insert(reply, {RequestKind::Object, object.satellite, object.key,
                             object.scanStartEpochSec});
    ++activeDownloads_;
}

void GlmSource::requestFinished(QNetworkReply* reply) {
    const auto it = requests_.find(reply);
    if (it == requests_.end()) {
        reply->deleteLater();
        return;
    }
    auto context = it.value();
    requests_.erase(it);
    if (context.kind == RequestKind::Listing) listingFinished(reply, context);
    else objectFinished(reply, std::move(context));
}

void GlmSource::listingFinished(QNetworkReply* reply, const RequestContext& context) {
    if (reply->error() == QNetworkReply::NoError) {
        QXmlStreamReader xml(reply->readAll());
        const auto nowEpoch = QDateTime::currentSecsSinceEpoch();
        // Keep a ten-minute overlap after startup. NOAA files are normally only
        // a few minutes late, but the overlap plus seen-key de-duplication makes
        // a transient delayed object/listing harmless instead of a permanent gap.
        const auto cutoff = nowEpoch - (initialBackfillComplete_
            ? 600 : static_cast<std::int64_t>(config_.initialBackfillMinutes) * 60);
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement() || xml.name() != QStringLiteral("Key")) continue;
            const auto key = xml.readElementText();
            const auto fileName = QFileInfo(key).fileName().toStdString();
            const auto start = parseGlmLcfaStartTime(fileName);
            if (!start || *start < cutoff || *start > nowEpoch + 60 || seenKeys_.contains(key)) continue;
            const auto latest = latestProcessedBySatellite_.value(
                context.satellite, std::numeric_limits<std::int64_t>::min());
            if (*start <= latest) continue;
            candidates_.push_back({context.satellite, key, *start});
        }
        if (xml.hasError()) ++batchFailures_;
    } else {
        ++batchFailures_;
    }
    reply->deleteLater();
    --pendingListings_;
    if (pendingListings_ == 0) beginDownloads();
}

void GlmSource::beginDownloads() {
    std::sort(candidates_.begin(), candidates_.end(), [](const auto& left, const auto& right) {
        if (left.scanStartEpochSec != right.scanStartEpochSec)
            return left.scanStartEpochSec < right.scanStartEpochSec;
        return left.satellite < right.satellite;
    });
    candidates_.erase(std::unique(candidates_.begin(), candidates_.end(), [](const auto& left, const auto& right) {
        return left.key == right.key;
    }), candidates_.end());
    for (const auto& object : candidates_) {
        downloadQueue_.enqueue(object);
        rememberSeenKey(object.key);
    }
    if (downloadQueue_.isEmpty()) {
        finishBatch();
        return;
    }
    emit statusChanged(QStringLiteral("downloading %1 new GLM files").arg(downloadQueue_.size()));
    pumpDownloads();
}

void GlmSource::pumpDownloads() {
    while (activeDownloads_ < config_.maximumConcurrentDownloads && !downloadQueue_.isEmpty())
        requestObject(downloadQueue_.dequeue());
    if (activeDownloads_ == 0 && pendingParses_ == 0 && downloadQueue_.isEmpty()) finishBatch();
}

void GlmSource::objectFinished(QNetworkReply* reply, RequestContext context) {
    --activeDownloads_;
    if (reply->error() != QNetworkReply::NoError) {
        ++batchFailures_;
        seenKeys_.remove(context.key);
        seenKeyOrder_.removeAll(context.key);
        reply->deleteLater();
        pumpDownloads();
        return;
    }
    auto data = reply->readAll();
    reply->deleteLater();
    ++pendingParses_;
    const QPointer<GlmSource> guard(this);
    processingPool_.start([guard, context = std::move(context), data = std::move(data)]() mutable {
        std::vector<LightningFlash> flashes;
        QString error;
        QTemporaryFile file(QDir::tempPath() + QStringLiteral("/cappedweather-glm-XXXXXX.nc"));
        if (!file.open() || file.write(data) != data.size() || !file.flush()) {
            error = QStringLiteral("could not stage GLM netCDF file");
        } else {
            const auto path = file.fileName();
            file.close();
            if (auto parsed = readGlmL2Lcfa(path.toStdString()); parsed.has_value()) {
                flashes = std::move(*parsed);
            } else {
                error = QStringLiteral("malformed GLM netCDF file");
            }
        }
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, context = std::move(context),
                                          flashes = std::move(flashes), error = std::move(error)]() mutable {
            if (guard) guard->finishParse(std::move(context), std::move(flashes), std::move(error));
        }, Qt::QueuedConnection);
    });
    pumpDownloads();
}

void GlmSource::finishParse(RequestContext context, std::vector<LightningFlash> flashes,
                            QString error) {
    --pendingParses_;
    if (!error.isEmpty()) {
        ++batchFailures_;
        seenKeys_.remove(context.key);
        seenKeyOrder_.removeAll(context.key);
    } else {
        parsedFiles_.push_back({context.satellite, context.key, context.scanStartEpochSec,
                                std::move(flashes)});
    }
    pumpDownloads();
}

void GlmSource::finishBatch() {
    if (!batchActive_) return;
    ingestBatch();
    batchActive_ = false;
    initialBackfillComplete_ = true;
    if (batchFailures_ > 0 && batchFilesProcessed_ == 0)
        emit requestFailed(QStringLiteral("GLM refresh failed (%1 listing/download/parse errors)")
                               .arg(batchFailures_));
    if (refreshPending_) QTimer::singleShot(0, this, &GlmSource::refresh);
}

void GlmSource::ingestBatch() {
    std::sort(parsedFiles_.begin(), parsedFiles_.end(), [](const auto& left, const auto& right) {
        if (left.scanStartEpochSec != right.scanStartEpochSec)
            return left.scanStartEpochSec < right.scanStartEpochSec;
        return left.satellite < right.satellite;
    });
    std::int64_t latestDataEpoch = 0;
    for (auto& file : parsedFiles_) {
        auto& latest = latestProcessedBySatellite_[file.satellite];
        if (file.scanStartEpochSec <= latest) continue;
        std::sort(file.flashes.begin(), file.flashes.end(), [](const auto& left, const auto& right) {
            return left.timeEpochSec < right.timeEpochSec;
        });
        for (const auto& flash : file.flashes) {
            if (flash.latitudeDeg < 15.0 || flash.latitudeDeg > 60.0 ||
                flash.longitudeDeg < -140.0 || flash.longitudeDeg > -50.0) continue;
            if (file.satellite == QStringLiteral("G19") &&
                flash.longitudeDeg < config_.satelliteSplitLongitudeDeg) continue;
            if (file.satellite == QStringLiteral("G18") &&
                flash.longitudeDeg >= config_.satelliteSplitLongitudeDeg) continue;
            grid_.addFlash(flash.timeEpochSec, flash.latitudeDeg, flash.longitudeDeg);
            ++batchFlashesProcessed_;
        }
        latest = file.scanStartEpochSec;
        latestDataEpoch = std::max(latestDataEpoch, file.scanStartEpochSec);
        ++batchFilesProcessed_;
    }

    const auto nowEpoch = QDateTime::currentSecsSinceEpoch();
    // Do not advance the detector to wall-clock time: LCFA arrives after the
    // flashes it contains, so doing that would make every later download look
    // "late" and silently discard it. Hold one current data bin open by polling
    // at the oldest satellite frontier; the next refresh closes that bin after
    // both East and West have supplied newer scans.
    std::int64_t detectorFrontier = 0;
    for (auto it = latestProcessedBySatellite_.cbegin();
         it != latestProcessedBySatellite_.cend(); ++it) {
        if (it.value() <= 0) continue;
        detectorFrontier = detectorFrontier == 0
            ? it.value() : std::min(detectorFrontier, it.value());
    }
    for (const auto& event : grid_.poll(detectorFrontier > 0 ? detectorFrontier : nowEpoch)) {
        LightningCellSnapshot snapshot;
        snapshot.kind = event.kind;
        snapshot.eventTimeEpochSec = event.timeEpochSec;
        snapshot.expiresEpochSec = event.kind == JumpEventKind::JumpCessation
            ? event.timeEpochSec + static_cast<std::int64_t>(config_.cessationRetentionMinutes) * 60
            : std::numeric_limits<std::int64_t>::max();
        snapshot.latitudeCenterDeg = (event.cellY + 0.5) * config_.grid.cellSizeDeg;
        snapshot.longitudeCenterDeg = (event.cellX + 0.5) * config_.grid.cellSizeDeg;
        snapshot.cellSizeDeg = config_.grid.cellSizeDeg;
        snapshot.sigmaLevel = event.sigmaLevel;
        snapshot.flashRatePerMin = event.flashRatePerMin;
        snapshot.cellX = event.cellX;
        snapshot.cellY = event.cellY;
        snapshot.satelliteNumber =
            snapshot.longitudeCenterDeg < config_.satelliteSplitLongitudeDeg ? 18 : 19;
        snapshot.latestDataEpochSec = latestProcessedBySatellite_.value(
            snapshot.satelliteNumber == 18 ? QStringLiteral("G18") : QStringLiteral("G19"));
        cells_.insert(cellKey(event.cellX, event.cellY), snapshot);
    }
    publishCells(nowEpoch);
    const auto ageSec = latestDataEpoch > 0 ? nowEpoch - latestDataEpoch : -1;
    const LightningCellSnapshot* strongest = nullptr;
    for (auto it = cells_.cbegin(); it != cells_.cend(); ++it) {
        if (it->active() && (!strongest || it->sigmaLevel > strongest->sigmaLevel))
            strongest = &it.value();
    }
    const auto strongestText = strongest
        ? QStringLiteral(" | strongest %1,%2 %3 sigma")
              .arg(strongest->latitudeCenterDeg, 0, 'f', 2)
              .arg(strongest->longitudeCenterDeg, 0, 'f', 2)
              .arg(strongest->sigmaLevel, 0, 'f', 1)
        : QString{};
    emit statusChanged(QStringLiteral("%1 GLM cells | %2 files, %3 flashes%4%5%6")
        .arg(cells_.size()).arg(batchFilesProcessed_).arg(batchFlashesProcessed_)
        .arg(ageSec >= 0 ? QStringLiteral(" | newest %1s ago").arg(ageSec) : QString{})
        .arg(strongestText)
        .arg(batchFailures_ ? QStringLiteral(" | %1 errors").arg(batchFailures_) : QString{}));
}

void GlmSource::publishCells(const std::int64_t nowEpochSec) {
    QVector<LightningCellSnapshot> snapshots;
    for (auto it = cells_.begin(); it != cells_.end();) {
        if (it->expiresEpochSec <= nowEpochSec) {
            it = cells_.erase(it);
        } else {
            const auto satellite = it->satelliteNumber == 18
                ? QStringLiteral("G18") : QStringLiteral("G19");
            it->latestDataEpochSec = latestProcessedBySatellite_.value(
                satellite, it->latestDataEpochSec);
            snapshots.push_back(it.value());
            ++it;
        }
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const auto& left, const auto& right) {
        if (left.active() != right.active()) return left.active() > right.active();
        if (left.sigmaLevel != right.sigmaLevel) return left.sigmaLevel > right.sigmaLevel;
        return left.eventTimeEpochSec > right.eventTimeEpochSec;
    });
    std::int64_t latest = 0;
    for (auto it = latestProcessedBySatellite_.cbegin(); it != latestProcessedBySatellite_.cend(); ++it)
        latest = std::max(latest, it.value());
    emit cellsUpdated(std::move(snapshots), QDateTime::fromSecsSinceEpoch(latest, Qt::UTC),
                      batchFilesProcessed_, batchFlashesProcessed_);
}

void GlmSource::rememberSeenKey(const QString& key) {
    if (seenKeys_.contains(key)) return;
    seenKeys_.insert(key);
    seenKeyOrder_.push_back(key);
    while (seenKeyOrder_.size() > 5000) seenKeys_.remove(seenKeyOrder_.takeFirst());
}

} // namespace cappedweather
