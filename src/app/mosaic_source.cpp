#include "app/mosaic_source.hpp"

#include "app/http_client.hpp"

#include <QBuffer>
#include <QImageReader>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimer>
#include <QTimeZone>

#include <algorithm>
#include <cstddef>

namespace cappedweather {
namespace {

// Paint budget: decimate so the frame stays near 100k cells (HRRR-scale).
constexpr qint64 kMaxPngBytes = 12 * 1024 * 1024;
constexpr std::size_t kMaxFrameCells = 100000;

QString twoDigits(int value) {
    return QStringLiteral("%1").arg(value, 2, 10, QLatin1Char('0'));
}

}  // namespace

QUrl mosaicListUrl(const QDate& dateUtc) {
    if (!dateUtc.isValid()) return {};
    return QUrl(QStringLiteral("https://mesonet.agron.iastate.edu/archive/data/%1/%2/%3/GIS/uscomp/")
                    .arg(dateUtc.year())
                    .arg(twoDigits(dateUtc.month()), twoDigits(dateUtc.day())));
}

QString newestMosaicFile(const QByteArray& listingHtml) {
    // href="n0q_202610101730.png": lexicographic max is chronological max
    // (fixed-width zero-padded timestamps).
    static const QRegularExpression pattern(QStringLiteral("n0q_\\d{12}\\.png"));
    QString best;
    auto it = pattern.globalMatch(QString::fromUtf8(listingHtml));
    while (it.hasNext()) {
        const auto match = it.next().captured();
        if (best.isEmpty() || match > best) best = match;
    }
    return best;
}

QDateTime latestMosaicSlot(const QDateTime& nowUtc) {
    if (!nowUtc.isValid()) return {};
    // Composites publish ~5 min late: back off two slots (10 min) for a
    // file that reliably exists, truncated to the 5-minute mark.
    const int minute = (nowUtc.time().minute() / 5) * 5;
    QDateTime slot(nowUtc.date(), QTime(nowUtc.time().hour(), minute), QTimeZone::UTC);
    return slot.addSecs(-2 * 300);
}

std::optional<MosaicFrame> decodeMosaicFrame(const QByteArray& png, const std::string& world,
                                             const long long validEpochSeconds, QString* error) {
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return std::optional<MosaicFrame>{};
    };
    if (png.isEmpty()) return fail(QStringLiteral("empty file"));
    // Scaled decode straight to paint budget: the full 12200x5400 would
    // cost 264 MB as RGBA32.
    QBuffer pngBuffer;
    pngBuffer.setData(png);
    pngBuffer.open(QIODevice::ReadOnly);
    QImageReader reader(&pngBuffer);
    const QSize full = reader.size();
    if (!full.isValid() || full.width() <= 0 || full.height() <= 0) return fail(QStringLiteral("bad image"));
    const auto cells = static_cast<double>(full.width()) * full.height();
    int stride = 1;
    while (cells / (stride * stride) > kMaxFrameCells) ++stride;
    reader.setScaledSize(
        QSize(std::max(1, full.width() / stride), std::max(1, full.height() / stride)));
    QImage image = reader.read();
    if (image.isNull()) return fail(QStringLiteral("unreadable file"));
    image = image.convertToFormat(QImage::Format_ARGB32);
    // Effective stride from actual sizes (scaled decode rounds): the grid
    // spacing follows the image in hand, never the requested size.
    const double strideX = static_cast<double>(full.width()) / image.width();
    const double strideY = static_cast<double>(full.height()) / image.height();
    auto grid = parseWorldFile(world, static_cast<std::size_t>(full.width()),
                               static_cast<std::size_t>(full.height()));
    if (!grid.has_value()) return fail(QStringLiteral("bad world file"));
    grid->degreesPerPixelX *= strideX;
    grid->degreesPerPixelY *= strideY;
    grid->nx = static_cast<std::size_t>(image.width());
    grid->ny = static_cast<std::size_t>(image.height());
    MosaicFrame frame;
    frame.validEpochSeconds = validEpochSeconds;
    frame.image = std::move(image);
    frame.grid = *grid;
    return frame;
}

MosaicSource::MosaicSource(QObject* parent) : QObject(parent) {
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(10 * 60 * 1000);
    refreshTimer_->setSingleShot(false);
    connect(refreshTimer_, &QTimer::timeout, this, &MosaicSource::refresh);
}

void MosaicSource::refresh() {
    if (!enabled_ || pending_) return;
    pending_ = true;
    if (!refreshTimer_->isActive()) refreshTimer_->start();
    requestDay(QDateTime::currentDateTimeUtc().date(), true);
}

void MosaicSource::setEnabled(const bool enabled) {
    enabled_ = enabled;
    if (!enabled_ && refreshTimer_) refreshTimer_->stop();
}

void MosaicSource::requestDay(const QDate& dateUtc, const bool fallbackLeft) {
    const QUrl url = mosaicListUrl(dateUtc);
    if (!url.isValid()) {
        pending_ = false;
        emit statusChanged(QStringLiteral("Mosaic: bad date"));
        return;
    }
    auto* reply = network_.get(
        makeRequest(url, QStringLiteral("text/html"), QStringLiteral(" mosaic list client")));
    connect(reply, &QNetworkReply::finished, this, [this, reply, dateUtc, fallbackLeft] {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const int code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto name = newestMosaicFile((ok && (code == 0 || (code >= 200 && code < 300)))
                                               ? reply->readAll()
                                               : QByteArray());
        if (name.isEmpty()) {
            // Today-then-yesterday, same fallback idiom as HMS smoke.
            if (fallbackLeft) {
                requestDay(dateUtc.addDays(-1), false);
                return;
            }
            pending_ = false;
            emit statusChanged(QStringLiteral("Mosaic: no composite listed"));
            return;
        }
        const QUrl base = mosaicListUrl(dateUtc).toString();
        auto* pngReply = network_.get(makeRequest(
            QUrl(base.toString() + name), QStringLiteral("image/png"), QStringLiteral(" mosaic client")));
        connect(pngReply, &QNetworkReply::finished, this, [this, pngReply, base, name] {
            pngReply->deleteLater();
            if (pngReply->error() != QNetworkReply::NoError) {
                pending_ = false;
                emit statusChanged(
                    QStringLiteral("Mosaic: %1").arg(pngReply->errorString()));
                return;
            }
            const QByteArray png = pngReply->readAll();
            if (png.size() > kMaxPngBytes || png.isEmpty()) {
                pending_ = false;
                emit statusChanged(QStringLiteral("Mosaic: file is empty or oversized"));
                return;
            }
            const QString worldName = name.left(name.size() - 4) + QStringLiteral(".wld");
            auto* wldReply = network_.get(makeRequest(
                QUrl(base.toString() + worldName), QStringLiteral("text/plain"),
                QStringLiteral(" mosaic world client")));
            connect(wldReply, &QNetworkReply::finished, this,
                    [this, wldReply, png, name] {
                        wldReply->deleteLater();
                        pending_ = false;
                        if (wldReply->error() != QNetworkReply::NoError) {
                            emit statusChanged(QStringLiteral("Mosaic: %1")
                                                   .arg(wldReply->errorString()));
                            return;
                        }
                        const auto epoch = parseMosaicTime(name.toStdString());
                        if (!epoch.has_value()) {
                            emit statusChanged(QStringLiteral("Mosaic: bad file time"));
                            return;
                        }
                        QString decodeError;
                        auto frame = decodeMosaicFrame(
                            png, QString::fromUtf8(wldReply->readAll()).toStdString(), *epoch,
                            &decodeError);
                        if (!frame.has_value()) {
                            emit statusChanged(
                                QStringLiteral("Mosaic: %1").arg(
                                    decodeError.isEmpty() ? QStringLiteral("unreadable file")
                                                          : decodeError));
                            return;
                        }
                        emit frameReady(frame);
                        const auto valid = QDateTime::fromSecsSinceEpoch(*epoch, QTimeZone::UTC);
                        emit statusChanged(
                            QStringLiteral("Mosaic: valid %1")
                                .arg(valid.isValid() ? valid.toString(QStringLiteral("HH:mm'Z'"))
                                                     : QStringLiteral("?")));
                    });
        });
    });
}

}  // namespace cappedweather
