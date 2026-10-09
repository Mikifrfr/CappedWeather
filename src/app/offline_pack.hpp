#pragma once

// Offline basemap pack for field chasing: pre-downloads raster tiles for a
// region/zoom range into the shared map-tile disk cache, so a no-signal
// chase still has roads. Tiles are fetched through the exact production
// URL builder (basemapTileUrls), and the packer writes into the exact
// production cache directory, so packed tiles are byte-identical to what
// live panning would have cached. Bounded by an explicit tile cap with a
// byte estimate up front; the user confirms before any download.

#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>

namespace cappedweather {

inline constexpr int kOfflinePackMaxTiles = 3000;  // explicit storage budget
inline constexpr int kOfflinePackParallel = 4;
inline constexpr qint64 kOfflinePackBytesPerTileUrl = 30 * 1024;  // PNG estimate

struct PackTile {
    int zoom = 0;
    int x = 0;
    int y = 0;
};

struct OfflinePackPlan {
    double latitude = 0.0;
    double longitude = 0.0;
    double halfDegrees = 0.0;
    int minZoom = 0;
    int maxZoom = 0;
    QVector<PackTile> tiles;
    int urlsPerTile = 1;  // Esri ships base + reference per tile
    qint64 estimatedBytes = 0;
    QString error;  // non-empty when the plan is unusable
};

// Pure tile math (Web Mercator, zooms clamped to the canvas 0..19 range).
// Fails closed on NaN input, inverted zooms, empty spans, antimeridian
// wrap, and anything over the tile cap: error is set and tiles is empty.
OfflinePackPlan planOfflinePack(double latitude, double longitude, double halfDegrees, int minZoom,
                                int maxZoom);

class OfflinePacker : public QObject {
    Q_OBJECT
public:
    explicit OfflinePacker(QObject* parent = nullptr);

    void start(const OfflinePackPlan& plan);
    void cancel();

signals:
    void progressed(int done, int total);
    void finished(bool ok, const QString& message);

private:
    void pump();
    void finishOne(QNetworkReply* reply);

    QNetworkAccessManager network_;
    QNetworkDiskCache* diskCache_ = nullptr;
    QQueue<QUrl> pending_;
    QSet<QNetworkReply*> active_;
    int total_ = 0;
    int done_ = 0;
    int failed_ = 0;
    qint64 bytes_ = 0;
    bool cancelled_ = false;
};

}  // namespace cappedweather
