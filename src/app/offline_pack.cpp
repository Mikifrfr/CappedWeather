#include "app/offline_pack.hpp"

#include "app/http_client.hpp"
#include "app/radar_map_canvas.hpp"

#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace cappedweather {
namespace {

constexpr double kMaxMercatorLatitude = 85.0511;
constexpr int kMaxTileZoom = 19;

int lonToTileX(const double longitude, const int zoom) {
    const int count = 1 << zoom;
    const int x = static_cast<int>(
        std::floor((longitude + 180.0) / 360.0 * static_cast<double>(count)));
    return std::clamp(x, 0, count - 1);
}

int latToTileY(const double latitude, const int zoom) {
    const int count = 1 << zoom;
    const double clamped = std::clamp(latitude, -kMaxMercatorLatitude, kMaxMercatorLatitude);
    const double radians = clamped * 3.14159265358979323846 / 180.0;
    const double mercator = std::log(std::tan(radians) + 1.0 / std::cos(radians));
    const int y = static_cast<int>(
        std::floor((1.0 - mercator / 3.14159265358979323846) / 2.0 * static_cast<double>(count)));
    return std::clamp(y, 0, count - 1);
}

QString sharedTileCacheDir() {
    // Same directory the canvas tile store uses: packed tiles land in the
    // live cache, so offline rendering needs no second code path.
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
           QStringLiteral("/map-tiles/shared");
}

}  // namespace

OfflinePackPlan planOfflinePack(const double latitude, const double longitude,
                                const double halfDegrees, const int minZoom, const int maxZoom) {
    OfflinePackPlan plan;
    plan.latitude = latitude;
    plan.longitude = longitude;
    plan.halfDegrees = halfDegrees;
    plan.minZoom = std::clamp(minZoom, 0, kMaxTileZoom);
    plan.maxZoom = std::clamp(maxZoom, 0, kMaxTileZoom);
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || !std::isfinite(halfDegrees)) {
        plan.error = QStringLiteral("Pack region is not finite");
        return plan;
    }
    if (halfDegrees <= 0.0 || halfDegrees > 20.0) {
        plan.error = QStringLiteral("Pack half-size must be 0–20 degrees");
        return plan;
    }
    if (plan.minZoom > plan.maxZoom) {
        plan.error = QStringLiteral("Minimum zoom is above maximum zoom");
        return plan;
    }
    if (longitude - halfDegrees < -180.0 || longitude + halfDegrees > 180.0) {
        // Single-region packs cannot wrap the antimeridian; a wrapped range
        // would silently pack the wrong ocean.
        plan.error = QStringLiteral("Pack region crosses the antimeridian");
        return plan;
    }
    for (int zoom = plan.minZoom; zoom <= plan.maxZoom; ++zoom) {
        const int xMin = lonToTileX(longitude - halfDegrees, zoom);
        const int xMax = lonToTileX(longitude + halfDegrees, zoom);
        const int yMin = latToTileY(latitude + halfDegrees, zoom);
        const int yMax = latToTileY(latitude - halfDegrees, zoom);
        for (int x = xMin; x <= xMax; ++x) {
            for (int y = yMin; y <= yMax; ++y) {
                plan.tiles.push_back({zoom, x, y});
                if (plan.tiles.size() > kOfflinePackMaxTiles) {
                    plan.tiles.clear();
                    plan.error = QStringLiteral("Region needs %1+ tiles: narrow it or drop a zoom")
                                     .arg(kOfflinePackMaxTiles);
                    return plan;
                }
            }
        }
    }
    if (plan.tiles.isEmpty()) {
        plan.error = QStringLiteral("Pack region covers no tiles");
        return plan;
    }
    // URL fan-out of the production builder: Esri ships a reference overlay
    // per tile, CARTO/custom ship one URL.
    const auto first = plan.tiles.constFirst();
    const auto urls = basemapTileUrls(first.zoom, first.x, first.y);
    plan.urlsPerTile = urls.reference.isEmpty() ? 1 : 2;
    plan.estimatedBytes = static_cast<qint64>(plan.tiles.size()) * plan.urlsPerTile *
                          kOfflinePackBytesPerTileUrl;
    return plan;
}

OfflinePacker::OfflinePacker(QObject* parent) : QObject(parent) {
    diskCache_ = new QNetworkDiskCache(this);
    diskCache_->setCacheDirectory(sharedTileCacheDir());
    network_.setCache(diskCache_);
}

void OfflinePacker::start(const OfflinePackPlan& plan) {
    cancelled_ = false;
    done_ = 0;
    failed_ = 0;
    bytes_ = 0;
    pending_.clear();
    if (!plan.error.isEmpty() || plan.tiles.isEmpty()) {
        emit finished(false, plan.error.isEmpty() ? QStringLiteral("Nothing to pack")
                                                  : plan.error);
        return;
    }
    for (const auto& tile : plan.tiles) {
        const auto urls = basemapTileUrls(tile.zoom, tile.x, tile.y);
        if (urls.base.isValid()) pending_.enqueue(urls.base);
        if (urls.reference.isValid()) pending_.enqueue(urls.reference);
    }
    total_ = static_cast<int>(pending_.size());
    if (total_ == 0) {
        emit finished(false, QStringLiteral("No tile URLs for this region"));
        return;
    }
    for (int i = 0; i < kOfflinePackParallel && !pending_.isEmpty(); ++i) pump();
}

void OfflinePacker::cancel() {
    cancelled_ = true;
    for (auto* reply : active_) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    active_.clear();
    pending_.clear();
    emit finished(false, QStringLiteral("Offline pack cancelled"));
}

void OfflinePacker::pump() {
    if (cancelled_ || pending_.isEmpty()) return;
    const QUrl url = pending_.dequeue();
    auto request = makeRequest(url, QString{}, QStringLiteral(" offline tile packer"));
    // AlwaysNetwork: a pack that only re-validates the cache downloads
    // nothing, which defeats the pre-chase purpose.
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    auto* reply = network_.get(request);
    active_.insert(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishOne(reply); });
}

void OfflinePacker::finishOne(QNetworkReply* reply) {
    reply->deleteLater();
    active_.remove(reply);
    if (cancelled_) return;
    if (reply->error() == QNetworkReply::NoError) {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 0 || (code >= 200 && code < 300)) {
            bytes_ += reply->readAll().size();
        } else {
            ++failed_;
        }
    } else {
        ++failed_;
    }
    ++done_;
    emit progressed(done_, total_);
    if (done_ >= total_) {
        // A pack where nothing landed is a failure, not an empty success:
        // the chase would drive out with no roads thinking it has maps.
        if (bytes_ == 0) {
            emit finished(false, QStringLiteral("No tiles downloaded (%1 failed): still online?")
                                             .arg(failed_));
            return;
        }
        emit finished(true, QStringLiteral("%1 tiles cached (%2 MB)%3")
                                   .arg(done_ - failed_)
                                   .arg(bytes_ / 1048576.0, 0, 'f', 1)
                                   .arg(failed_ > 0 ? QStringLiteral(", %1 failed").arg(failed_)
                                                    : QString{}));
        return;
    }
    pump();
}

}  // namespace cappedweather
