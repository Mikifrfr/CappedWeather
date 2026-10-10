#include "app/hrrr_source.hpp"

#include "app/http_client.hpp"

#include <QNetworkReply>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QTimeZone>

#include <cmath>

namespace cappedweather {
namespace {

constexpr char kHrrrNcss[] =
    "https://thredds.ucar.edu/thredds/ncss/grid/grib/NCEP/HRRR/CONUS_2p5km/Best";
constexpr char kHrrrVar[] = "Composite_reflectivity_entire_atmosphere";
// CONUS coverage from the TDS geospatial record; boxes clamp inside it.
constexpr double kMinLat = 20.0;
constexpr double kMaxLat = 51.0;
constexpr double kMinLon = -131.0;
constexpr double kMaxLon = -60.0;
constexpr double kMinHalfDeg = 1.0;
constexpr double kMaxHalfDeg = 8.0;
constexpr qint64 kMaxSubsetBytes = 8 * 1024 * 1024;

QString isoTime(const QDateTime& moment) {
    return moment.toString(QStringLiteral("yyyy-MM-ddThh:mm:ss'Z'"));
}

}  // namespace

QDateTime latestHrrrTime(const QDateTime& nowUtc) {
    if (!nowUtc.isValid()) return {};
    const QDateTime truncated(nowUtc.date(), QTime(nowUtc.time().hour(), 0), QTimeZone::UTC);
    // Analyses land ~1 h late; two hours back is reliably published.
    return truncated.addSecs(-2 * 3600);
}

QUrl hrrrSubsetUrl(const double centerLatitude, const double centerLongitude,
                   const double halfDegrees, const QDateTime& validTimeUtc) {
    if (!std::isfinite(centerLatitude) || !std::isfinite(centerLongitude) ||
        !std::isfinite(halfDegrees) || !validTimeUtc.isValid())
        return {};
    const double half = std::clamp(halfDegrees, kMinHalfDeg, kMaxHalfDeg);
    const double north = std::min(kMaxLat, centerLatitude + half);
    const double south = std::max(kMinLat, centerLatitude - half);
    const double east = std::min(kMaxLon, centerLongitude + half);
    const double west = std::max(kMinLon, centerLongitude - half);
    if (!(north > south) || !(east > west)) return {};
    return QUrl(QString::fromLatin1(kHrrrNcss) +
                QStringLiteral("?var=%1&north=%2&south=%3&east=%4&west=%5&time=%6&accept=netCDF")
                    .arg(QString::fromLatin1(kHrrrVar))
                    .arg(north, 0, 'f', 3)
                    .arg(south, 0, 'f', 3)
                    .arg(east, 0, 'f', 3)
                    .arg(west, 0, 'f', 3)
                    .arg(isoTime(validTimeUtc)));
}

HrrrSource::HrrrSource(QObject* parent) : QObject(parent) {
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(30 * 60 * 1000);
    refreshTimer_->setSingleShot(false);
    connect(refreshTimer_, &QTimer::timeout, this, &HrrrSource::refresh);
}

void HrrrSource::refresh() {
    if (!enabled_ || pending_) return;
    pending_ = true;
    if (!refreshTimer_->isActive()) refreshTimer_->start();
    requestTime(latestHrrrTime(QDateTime::currentDateTimeUtc()), true);
}

void HrrrSource::setEnabled(const bool enabled) {
    enabled_ = enabled;
    if (!enabled_ && refreshTimer_) refreshTimer_->stop();
}

void HrrrSource::setRegion(const double latitude, const double longitude,
                           const double halfDegrees) {
    if (!std::isfinite(latitude) || !std::isfinite(longitude)) return;
    regionLatitude_ = std::clamp(latitude, -90.0, 90.0);
    regionLongitude_ = std::clamp(longitude, -180.0, 180.0);
    regionHalfDegrees_ = std::clamp(halfDegrees, kMinHalfDeg, kMaxHalfDeg);
}

void HrrrSource::requestTime(const QDateTime& validTime, const bool fallbackLeft) {
    const QUrl url = hrrrSubsetUrl(regionLatitude_, regionLongitude_, regionHalfDegrees_,
                                   validTime);
    if (!url.isValid()) {
        pending_ = false;
        emit statusChanged(QStringLiteral("HRRR: bad region or time"));
        return;
    }
    auto* reply = network_.get(
        makeRequest(url, QStringLiteral("application/x-netcdf"), QStringLiteral(" HRRR client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, validTime, fallbackLeft] {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const int code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool httpOk = ok && (code == 0 || (code >= 200 && code < 300));
        if (!httpOk) {
            // One step backoff: the newest analysis may not be ingested yet.
            if (fallbackLeft && validTime.isValid()) {
                requestTime(validTime.addSecs(-3600), false);
                return;
            }
            pending_ = false;
            emit statusChanged(QStringLiteral("HRRR: %1")
                                   .arg(ok ? QStringLiteral("HTTP %1").arg(code)
                                           : reply->errorString()));
            return;
        }
        const QByteArray payload = reply->readAll();
        if (payload.size() > kMaxSubsetBytes || payload.isEmpty()) {
            pending_ = false;
            emit statusChanged(QStringLiteral("HRRR: subset is empty or oversized"));
            return;
        }
        // netCDF-C reads paths, not memory: stage through a temp dir.
        QTemporaryDir staging;
        QString error;
        std::optional<HrrrFrame> frame;
        if (staging.isValid()) {
            const QString path = staging.filePath(QStringLiteral("hrrr_refc.nc"));
            QFile file(path);
            if (file.open(QIODevice::WriteOnly) && file.write(payload) == payload.size()) {
                file.close();
                frame = readHrrrRefc(path.toStdString());
            } else {
                error = QStringLiteral("could not stage the subset");
            }
        } else {
            error = QStringLiteral("no temp space for the subset");
        }
        pending_ = false;
        if (!error.isEmpty() || !frame.has_value()) {
            emit statusChanged(QStringLiteral("HRRR: %1")
                                   .arg(!error.isEmpty() ? error
                                                         : QStringLiteral("unreadable subset")));
            return;
        }
        emit frameReady(frame);
        const auto valid =
            QDateTime::fromSecsSinceEpoch(frame->validEpochSeconds, QTimeZone::UTC);
        emit statusChanged(QStringLiteral("HRRR: valid %1").arg(
            valid.isValid() ? valid.toString(QStringLiteral("HH:mm'Z'")) : QStringLiteral("?")));
    });
}

}  // namespace cappedweather
