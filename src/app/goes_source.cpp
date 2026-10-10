#include "app/goes_source.hpp"

#include "app/http_client.hpp"
#include "app/s3_listing.hpp"

#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>
#include <QTimeZone>
#include <QFile>

#include <cmath>

namespace cappedweather {
namespace {

constexpr qint64 kMaxRadBytes = 12 * 1024 * 1024;  // C13/C07 run ~4 MB; C02 refused by channel gate

}  // namespace

bool goesChannelSupported(const int channel) {
    return channel == 13 || channel == 7;
}

QUrl goesListUrl(const QString& bucket, const QDateTime& hourUtc) {
    if (bucket.trimmed().isEmpty() || !hourUtc.isValid()) return {};
    const QDate date = hourUtc.date();
    return QUrl(QStringLiteral("https://%1.s3.amazonaws.com/?list-type=2&prefix=ABI-L1b-RadC/%2/%3/%4/&max-keys=300")
                    .arg(bucket.trimmed(),
                         QString::number(date.year()),
                         QString::number(date.dayOfYear()),
                         QStringLiteral("%1").arg(hourUtc.time().hour(), 2, 10, QLatin1Char('0'))));
}

QString newestGoesKey(const QByteArray& listingXml, const int channel) {
    if (!goesChannelSupported(channel)) return {};
    QString error;
    const auto listing = parseS3Listing(listingXml, &error);
    if (!error.isEmpty()) return {};
    const auto tag = QStringLiteral("M6C%1").arg(channel, 2, 10, QLatin1Char('0'));
    QString best;
    for (const auto& key : listing.keys) {
        if (!key.contains(tag)) continue;
        if (best.isEmpty() || key > best) best = key;
    }
    return best;
}

QDateTime latestGoesSlot(const QDateTime& nowUtc) {
    if (!nowUtc.isValid()) return {};
    // Scans publish ~5 min late: back off two slots (10 min) for a file
    // that reliably exists, truncated to the 5-minute mark.
    const int minute = (nowUtc.time().minute() / 5) * 5;
    QDateTime slot(nowUtc.date(), QTime(nowUtc.time().hour(), minute), QTimeZone::UTC);
    return slot.addSecs(-2 * 300);
}

GoesSource::GoesSource(QObject* parent) : QObject(parent) {
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(10 * 60 * 1000);
    refreshTimer_->setSingleShot(false);
    connect(refreshTimer_, &QTimer::timeout, this, &GoesSource::refresh);
}

void GoesSource::refresh() {
    if (!enabled_ || pending_) return;
    if (!goesChannelSupported(channel_)) return;
    pending_ = true;
    if (!refreshTimer_->isActive()) refreshTimer_->start();
    const auto slot = latestGoesSlot(QDateTime::currentDateTimeUtc());
    requestHour(QDateTime(slot.date(), QTime(slot.time().hour(), 0), QTimeZone::UTC), true);
}

void GoesSource::setEnabled(const bool enabled) {
    enabled_ = enabled;
    if (!enabled_ && refreshTimer_) refreshTimer_->stop();
}

void GoesSource::setChannel(const int channel) {
    if (!goesChannelSupported(channel) || channel_ == channel) return;
    channel_ = channel;
    if (enabled_) {
        pending_ = false;
        refresh();
    }
}

void GoesSource::requestHour(const QDateTime& hourUtc, const bool fallbackLeft) {
    const QUrl url = goesListUrl(bucket_, hourUtc);
    if (!url.isValid()) {
        pending_ = false;
        emit statusChanged(QStringLiteral("GOES: bad bucket or hour"));
        return;
    }
    auto* reply = network_.get(
        makeRequest(url, QStringLiteral("application/xml"), QStringLiteral(" GOES list client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, hourUtc, fallbackLeft] {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const int code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!ok || (code != 0 && (code < 200 || code >= 300))) {
            if (fallbackLeft) {
                requestHour(hourUtc.addSecs(-3600), false);
                return;
            }
            pending_ = false;
            emit statusChanged(QStringLiteral("GOES list: %1")
                                   .arg(ok ? QStringLiteral("HTTP %1").arg(code)
                                           : reply->errorString()));
            return;
        }
        const auto key = newestGoesKey(reply->readAll(), channel_);
        if (key.isEmpty()) {
            if (fallbackLeft) {
                requestHour(hourUtc.addSecs(-3600), false);
                return;
            }
            pending_ = false;
            emit statusChanged(QStringLiteral("GOES: no C%1 file this hour").arg(channel_));
            return;
        }
        requestFile(QUrl(QStringLiteral("https://%1.s3.amazonaws.com/%2").arg(bucket_, key)));
    });
}

void GoesSource::requestFile(const QUrl& fileUrl) {
    auto* reply = network_.get(
        makeRequest(fileUrl, QStringLiteral("application/octet-stream"), QStringLiteral(" GOES client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, fileUrl] {
        reply->deleteLater();
        pending_ = false;
        if (reply->error() != QNetworkReply::NoError) {
            emit statusChanged(
                QStringLiteral("GOES: %1").arg(reply->errorString()));
            return;
        }
        const QByteArray payload = reply->readAll();
        if (payload.size() > kMaxRadBytes || payload.isEmpty()) {
            emit statusChanged(QStringLiteral("GOES: file is empty or oversized"));
            return;
        }
        QTemporaryDir staging;
        QString error;
        std::optional<GoesFrame> frame;
        if (staging.isValid()) {
            const QString path = staging.filePath(QStringLiteral("goes_rad.nc"));
            QFile file(path);
            if (file.open(QIODevice::WriteOnly) && file.write(payload) == payload.size()) {
                file.close();
                frame = readGoesRad(path.toStdString());
            } else {
                error = QStringLiteral("could not stage the file");
            }
        } else {
            error = QStringLiteral("no temp space for the file");
        }
        if (!error.isEmpty() || !frame.has_value()) {
            emit statusChanged(QStringLiteral("GOES: %1")
                                   .arg(!error.isEmpty() ? error
                                                         : QStringLiteral("unreadable file")));
            return;
        }
        emit frameReady(frame);
        const auto valid =
            QDateTime::fromSecsSinceEpoch(frame->validEpochSeconds, QTimeZone::UTC);
        emit statusChanged(QStringLiteral("GOES C%1: valid %2")
                               .arg(frame->channel)
                               .arg(valid.isValid() ? valid.toString(QStringLiteral("HH:mm'Z'"))
                                                    : QStringLiteral("?")));
    });
}

}  // namespace cappedweather
