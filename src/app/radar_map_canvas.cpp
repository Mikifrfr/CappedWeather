#include "app/radar_map_canvas.hpp"

#include "core/radar_geometry.hpp"
#include "core/scan_diagnostics.hpp"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkDiskCache>
#include <QApplication>
#include <QCache>
#include <QPointer>
#include <QLineF>
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QThreadPool>
#include <QToolTip>
#include <QStandardPaths>
#include <QSettings>
#include <QSet>
#include <QShowEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <mutex>

namespace cappedweather {

namespace {
constexpr double kPi = 3.14159265358979323846;

struct ColorStop {
    double position;
    QColor color;
};

struct ValueColorStop {
    double minimum;
    QColor color;
};

QColor discretePalette(const std::initializer_list<ValueColorStop> stops, const double value,
                       const int opacity) {
    auto selected = stops.begin()->color;
    for (const auto& stop : stops) {
        if (value < stop.minimum) break;
        selected = stop.color;
    }
    selected.setAlpha(opacity);
    return selected;
}

QColor interpolatePalette(const std::initializer_list<ColorStop> stops, const double position,
                          const int opacity) {
    const auto value = std::clamp(position, 0.0, 1.0);
    auto previous = stops.begin();
    for (auto current = std::next(previous); current != stops.end(); ++current) {
        if (value > current->position) {
            previous = current;
            continue;
        }
        const auto span = current->position - previous->position;
        const auto amount = span > 0.0 ? (value - previous->position) / span : 0.0;
        const auto channel = [amount](const int from, const int to) {
            return static_cast<int>(std::round(from + (to - from) * amount));
        };
        return QColor(channel(previous->color.red(), current->color.red()),
                      channel(previous->color.green(), current->color.green()),
                      channel(previous->color.blue(), current->color.blue()), opacity);
    }
    auto result = std::prev(stops.end())->color;
    result.setAlpha(opacity);
    return result;
}

QColor productColor(const ProductKind product, const double normalized,
                    const RadarMapCanvas::PaletteKind palette, const int opacityPercent,
                    const QVector<RadarMapCanvas::PaletteStop>& custom = {}) {
    const auto alpha = std::clamp(static_cast<int>(std::round(opacityPercent * 2.55)), 0, 255);
    if (palette == RadarMapCanvas::PaletteKind::Custom && !custom.isEmpty()) {
        const auto& descriptor = productDescriptor(product);
        const auto value = descriptor.minimum + normalized * (descriptor.maximum - descriptor.minimum);
        auto color = custom.first().color;
        for (const auto& stop : custom) {
            if (value < stop.value) break;
            color = stop.color;
        }
        color.setAlpha(alpha);
        return color;
    }
    if (palette == RadarMapCanvas::PaletteKind::ColorSafe) {
        if (product == ProductKind::Velocity || product == ProductKind::DifferentialReflectivity ||
            product == ProductKind::DifferentialPhase) {
            return interpolatePalette({
                {0.00, QColor("#3b4cc0")}, {0.25, QColor("#78a5d5")},
                {0.50, QColor("#f1f1f1")}, {0.75, QColor("#e89c75")},
                {1.00, QColor("#b40426")},
            }, normalized, alpha);
        }
        return interpolatePalette({
            {0.00, QColor("#440154")}, {0.25, QColor("#3b528b")},
            {0.50, QColor("#21918c")}, {0.75, QColor("#5ec962")},
            {1.00, QColor("#fde725")},
        }, normalized, alpha);
    }
    if (palette == RadarMapCanvas::PaletteKind::Operational) {
        const auto& descriptor = productDescriptor(product);
        const auto value = descriptor.minimum + normalized * (descriptor.maximum - descriptor.minimum);
        switch (product) {
        case ProductKind::Reflectivity:
            return discretePalette({
                {-32, QColor("#646464")}, {-10, QColor("#04e9e7")}, {5, QColor("#019ff4")},
                {15, QColor("#0300f4")}, {20, QColor("#02fd02")}, {30, QColor("#008e00")},
                {40, QColor("#fdf802")}, {50, QColor("#fd9500")}, {55, QColor("#fd0000")},
                {65, QColor("#bc00bc")}, {75, QColor("#f8f8f8")},
            }, value, alpha);
        case ProductKind::Velocity:
            return discretePalette({
                {-80, QColor("#640064")}, {-50, QColor("#9b009b")}, {-35, QColor("#003cff")},
                {-20, QColor("#00a0ff")}, {-10, QColor("#00e6c8")}, {-2, QColor("#b8fff0")},
                {2, QColor("#f2f2f2")}, {10, QColor("#ffe000")}, {20, QColor("#ff7800")},
                {35, QColor("#e60000")}, {50, QColor("#960000")},
            }, value, alpha);
        case ProductKind::SpectrumWidth:
            return discretePalette({
                {0, QColor("#202a30")}, {2, QColor("#0060a8")}, {5, QColor("#00a9c7")},
                {8, QColor("#36c56a")}, {12, QColor("#d8d33c")}, {18, QColor("#f28b32")},
                {25, QColor("#db3657")}, {32, QColor("#f0e9ff")},
            }, value, alpha);
        case ProductKind::DifferentialReflectivity:
            return discretePalette({
                {-8, QColor("#4b2678")}, {-3, QColor("#2456b8")}, {-1, QColor("#3fb5ca")},
                {0, QColor("#d9e1df")}, {1, QColor("#f0c74d")}, {2, QColor("#ef8c3d")},
                {4, QColor("#dc3e70")}, {6, QColor("#8b2d91")},
            }, value, alpha);
        case ProductKind::CorrelationCoefficient:
            return discretePalette({
                {0.00, QColor("#4c1647")}, {0.70, QColor("#a72f4f")}, {0.85, QColor("#e06b4f")},
                {0.92, QColor("#d6b74a")}, {0.95, QColor("#7374c8")}, {0.97, QColor("#27a7b6")},
                {0.985, QColor("#91ddd0")}, {0.995, QColor("#f3fbf7")},
            }, value, alpha);
        case ProductKind::DifferentialPhase:
            return discretePalette({
                {-180, QColor("#403078")}, {-120, QColor("#2858a0")}, {-60, QColor("#20a7b0")},
                {0, QColor("#73c95b")}, {45, QColor("#e0cf48")}, {90, QColor("#ed7b45")},
                {135, QColor("#b94399")},
            }, value, alpha);
        }
    }
    switch (product) {
    case ProductKind::Reflectivity:
        return interpolatePalette({
            {0.00, QColor("#102b3a")}, {0.16, QColor("#146b78")},
            {0.30, QColor("#35c7b4")}, {0.44, QColor("#b7e75c")},
            {0.58, QColor("#ffd166")}, {0.72, QColor("#ff704d")},
            {0.86, QColor("#d946a8")}, {1.00, QColor("#f2e9ff")},
        }, normalized, alpha);
    case ProductKind::Velocity:
        return interpolatePalette({
            {0.00, QColor("#38156b")}, {0.18, QColor("#304fc4")},
            {0.36, QColor("#16a6d8")}, {0.48, QColor("#63eee0")},
            {0.495, QColor("#24343b")}, {0.505, QColor("#24343b")},
            {0.52, QColor("#ffe07a")}, {0.66, QColor("#ff9b42")},
            {0.84, QColor("#ef3f55")}, {1.00, QColor("#9e185f")},
        }, normalized, alpha);
    case ProductKind::SpectrumWidth:
        return interpolatePalette({
            {0.00, QColor("#17252d")}, {0.16, QColor("#14637c")},
            {0.32, QColor("#16b6c5")}, {0.48, QColor("#66d36e")},
            {0.64, QColor("#e7d44f")}, {0.78, QColor("#f58a3c")},
            {0.90, QColor("#d9365e")}, {1.00, QColor("#f4eaff")},
        }, normalized, alpha);
    case ProductKind::DifferentialReflectivity:
        return interpolatePalette({
            {0.00, QColor("#392568")}, {0.22, QColor("#3978b8")},
            {0.42, QColor("#54c8bd")}, {0.50, QColor("#dbe4df")},
            {0.66, QColor("#efb45d")}, {0.84, QColor("#e45a78")},
            {1.00, QColor("#9f3a96")},
        }, normalized, alpha);
    case ProductKind::CorrelationCoefficient:
        return interpolatePalette({
            {0.00, QColor("#76264c")}, {0.35, QColor("#d5525c")},
            {0.62, QColor("#e8a957")}, {0.78, QColor("#7779c9")},
            {0.90, QColor("#2eb7bd")}, {0.97, QColor("#a8e4d7")},
            {1.00, QColor("#edf7f2")},
        }, normalized, alpha);
    case ProductKind::DifferentialPhase:
        return interpolatePalette({
            {0.00, QColor("#234c89")}, {0.18, QColor("#2cb5b4")},
            {0.36, QColor("#9cd65f")}, {0.52, QColor("#f2cf58")},
            {0.68, QColor("#ee7454")}, {0.84, QColor("#bd4ca5")},
            {1.00, QColor("#51418d")},
        }, normalized, alpha);
    }
    return QColor("#d7e2e6");
}

int wrappedTileX(const int x, const int count) {
    return ((x % count) + count) % count;
}

struct SharedTileStore {
    QNetworkAccessManager* network = nullptr;
    QNetworkDiskCache* diskCache = nullptr;
    QCache<QString, QImage> tiles;
    QSet<QString> pendingKeys;
    QHash<QNetworkReply*, QString> replies;
    QHash<QString, QVector<QPointer<RadarMapCanvas>>> watchers;
};

void watchTile(SharedTileStore& store, const QString& key, RadarMapCanvas* canvas) {
    auto& watchers = store.watchers[key];
    const auto alreadyWatching = std::any_of(watchers.cbegin(), watchers.cend(),
                                              [canvas](const auto& watcher) {
                                                  return watcher == canvas;
                                              });
    if (!alreadyWatching) watchers.push_back(canvas);
}

SharedTileStore& tileStore() {
    static auto* store = [] {
        auto* result = new SharedTileStore;
        const QSettings settings;
        result->tiles.setMaxCost(std::clamp(
            settings.value(QStringLiteral("cache/memoryMbPerPanel"), 32).toInt(), 8, 256) * 1024 * 1024);
        result->network = new QNetworkAccessManager(qApp);
        result->diskCache = new QNetworkDiskCache(result->network);
        result->diskCache->setCacheDirectory(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/map-tiles/shared"));
        result->diskCache->setMaximumCacheSize(std::clamp<qint64>(
            settings.value(QStringLiteral("cache/diskMbPerPanel"), 64).toLongLong(), 16, 1024) * 1024LL * 1024LL);
        result->network->setCache(result->diskCache);
        QObject::connect(result->network, &QNetworkAccessManager::finished,
                         result->network, [result](QNetworkReply* reply) {
            const auto key = result->replies.take(reply);
            result->pendingKeys.remove(key);
            if (reply->error() == QNetworkReply::NoError) {
                const auto image = QImage::fromData(reply->readAll(), "PNG");
                if (!image.isNull()) {
                    const auto cost = static_cast<int>(std::min<qsizetype>(
                        image.sizeInBytes(), std::numeric_limits<int>::max()));
                    result->tiles.insert(key, new QImage(image), std::max(1, cost));
                }
            }
            reply->deleteLater();
            const auto watchers = result->watchers.take(key);
            for (const auto& canvas : watchers)
                if (canvas && canvas->isVisible()) canvas->update();
        });
        return result;
    }();
    return *store;
}

struct VelocityCacheEntry {
    std::weak_ptr<const DecodedChunk> chunk;
    std::array<int, 4> parameters{};
    std::weak_ptr<const ProcessedVelocity> processed;
    std::shared_future<std::shared_ptr<const ProcessedVelocity>> pending;
};

std::mutex velocityCacheMutex;
std::vector<VelocityCacheEntry> velocityCache;

std::array<int, 4> velocityCacheParameters(const VelocityProcessingOptions& options) {
    return {static_cast<int>(options.mode),
            static_cast<int>(std::lround(options.nyquistVelocityMetersPerSecond * 10.0)),
            static_cast<int>(std::lround(options.stormMotionDirectionDegrees * 10.0)),
            static_cast<int>(std::lround(options.stormMotionSpeedMetersPerSecond * 10.0))};
}

bool sameOwner(const std::weak_ptr<const DecodedChunk>& cached,
               const std::shared_ptr<const DecodedChunk>& chunk) {
    const std::owner_less<void> less;
    return !less(cached, chunk) && !less(chunk, cached);
}

std::shared_ptr<const ProcessedVelocity> sharedProcessedVelocity(
    const std::shared_ptr<const DecodedChunk>& chunk, const VelocityProcessingOptions& options) {
    const auto parameters = velocityCacheParameters(options);
    std::shared_future<std::shared_ptr<const ProcessedVelocity>> pending;
    std::shared_ptr<std::promise<std::shared_ptr<const ProcessedVelocity>>> producer;
    {
        const std::scoped_lock lock(velocityCacheMutex);
        std::erase_if(velocityCache, [](const auto& entry) {
            return entry.chunk.expired() && !entry.pending.valid();
        });
        const auto existing = std::find_if(velocityCache.begin(), velocityCache.end(),
                                           [&chunk, &parameters](const auto& entry) {
                                               return entry.parameters == parameters &&
                                                      sameOwner(entry.chunk, chunk);
                                           });
        if (existing != velocityCache.end()) {
            if (const auto processed = existing->processed.lock()) return processed;
            if (existing->pending.valid()) pending = existing->pending;
        }
        if (!pending.valid()) {
            producer = std::make_shared<std::promise<std::shared_ptr<const ProcessedVelocity>>>();
            pending = producer->get_future().share();
            if (existing != velocityCache.end()) existing->pending = pending;
            else velocityCache.push_back({chunk, parameters, {}, pending});
        }
    }
    if (!producer) return pending.get();
    auto processed = std::make_shared<const ProcessedVelocity>(processVelocity(*chunk, options));
    {
        const std::scoped_lock lock(velocityCacheMutex);
        const auto existing = std::find_if(velocityCache.begin(), velocityCache.end(),
                                           [&chunk, &parameters](const auto& entry) {
                                               return entry.parameters == parameters &&
                                                      sameOwner(entry.chunk, chunk);
                                           });
        if (existing != velocityCache.end()) {
            existing->processed = processed;
            existing->pending = {};
        }
        if (velocityCache.size() > 64) {
            std::erase_if(velocityCache, [](const auto& entry) {
                return entry.chunk.expired() && !entry.pending.valid();
            });
        }
    }
    producer->set_value(processed);
    return processed;
}
}

RadarMapCanvas::RadarMapCanvas(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("radarMap"));
    setMinimumSize(300, 220);
    setMouseTracking(true);
    loadingTimer_ = new QTimer(this);
    loadingTimer_->setInterval(80);
    connect(loadingTimer_, &QTimer::timeout, this, [this] {
        loadingAngle_ = (loadingAngle_ + 18) % 360;
        update(QRect(width() - 300, 58, 294, 52));
    });
    probeTimer_ = new QTimer(this);
    probeTimer_->setSingleShot(true);
    probeTimer_->setInterval(16);
    connect(probeTimer_, &QTimer::timeout, this, [this] {
        hoverRadarId_ = radarAt(pendingProbePosition_);
        updateProbe(pendingProbePosition_);
    });
}

RadarMapCanvas::~RadarMapCanvas() = default;

void RadarMapCanvas::setSites(std::vector<RadarSite> sites) {
    sites_ = std::move(sites);
    siteIndexes_.clear();
    for (std::size_t index = 0; index < sites_.size(); ++index)
        siteIndexes_.insert(QString::fromStdString(sites_[index].id), index);
    siteWorldZoom_ = -1;
    invalidateOverlay(true);
    update();
}

void RadarMapCanvas::setSelectedRadar(const QString& radarId) {
    const auto changed = selectedRadar_ != radarId;
    selectedRadar_ = radarId;
    if (changed) {
        chunk_.reset();
        invalidateOverlay();
        dataTime_ = {};
        probeText_.clear();
        const auto* site = selectedSite();
        if (!site || !site->latitude || !site->longitude) {
            update();
            return;
        }
        center_ = {site->longitude.value(), site->latitude.value()};
        zoom_ = 6;
        invalidateWorldCaches(true);
    }
    update();
}

void RadarMapCanvas::setProduct(const ProductKind product) {
    product_ = product;
    refreshVelocityProcessing();
    rebuildProbeIndex();
    invalidateOverlay(true);
    refreshScanDiagnostics();
    update();
}

void RadarMapCanvas::setElevationFilter(const std::optional<int> elevationNumber) {
    elevationFilter_ = elevationNumber;
    rebuildProbeIndex();
    invalidateOverlay();
    refreshScanDiagnostics();
    update();
}

void RadarMapCanvas::setDecodedChunk(DecodedChunk chunk, const QDateTime& dataTime) {
    setDecodedChunk(std::make_shared<DecodedChunk>(std::move(chunk)), dataTime);
}

void RadarMapCanvas::setDecodedChunk(std::shared_ptr<const DecodedChunk> chunk, const QDateTime& dataTime) {
    chunk_ = std::move(chunk);
    dataTime_ = dataTime;
    invalidateOverlay();
    refreshVelocityProcessing();
    rebuildProbeIndex();
    refreshScanDiagnostics();
    setLoading(false);
    update();
}

void RadarMapCanvas::setWeatherOverlays(QVector<WeatherWarning> warnings,
                                        QVector<StormReport> reports) {
    warnings_ = std::move(warnings);
    stormReports_ = std::move(reports);
    ++weatherGeneration_;
    update();
}

void RadarMapCanvas::setWarningsVisible(const bool visible) {
    warningsVisible_ = visible;
    update();
}

void RadarMapCanvas::setStormReportsVisible(const bool visible) {
    stormReportsVisible_ = visible;
    update();
}

void RadarMapCanvas::setStormReportKindVisible(const StormReportKind kind, const bool visible) {
    stormReportKindsVisible_.at(static_cast<std::size_t>(kind)) = visible;
    update();
}

void RadarMapCanvas::setWarningFilter(const QString& filter) {
    warningFilter_ = filter;
    ++weatherGeneration_;
    update();
}

void RadarMapCanvas::setPalette(const PaletteKind palette) {
    palette_ = palette;
    invalidateOverlay();
    update();
}

void RadarMapCanvas::setCustomPalette(QVector<PaletteStop> stops) {
    std::sort(stops.begin(), stops.end(), [](const auto& left, const auto& right) {
        return left.value < right.value;
    });
    customPalette_ = std::move(stops);
    invalidateOverlay();
}

void RadarMapCanvas::setRadarOpacity(const int percent) {
    radarOpacity_ = std::clamp(percent, 10, 100);
    invalidateOverlay();
    update();
}

void RadarMapCanvas::setBelowThresholdVisible(const bool visible) {
    belowThresholdVisible_ = visible;
    invalidateOverlay();
    update();
}

void RadarMapCanvas::setVelocityProcessing(VelocityProcessingOptions options) {
    options.nyquistVelocityMetersPerSecond = std::clamp(options.nyquistVelocityMetersPerSecond, 1.0, 80.0);
    options.stormMotionDirectionDegrees = std::fmod(options.stormMotionDirectionDegrees + 360.0, 360.0);
    options.stormMotionSpeedMetersPerSecond = std::clamp(options.stormMotionSpeedMetersPerSecond, 0.0, 80.0);
    velocityOptions_ = options;
    refreshVelocityProcessing();
    invalidateOverlay();
    update();
}

void RadarMapCanvas::refreshVelocityProcessing() {
    processedVelocity_.reset();
    const auto generation = ++velocityGeneration_;
    velocityBuildPending_ = false;
    if (!isVisible()) return;
    if (!chunk_ || product_ != ProductKind::Velocity ||
        velocityOptions_.mode == VelocityProcessingMode::Native) return;
    velocityBuildPending_ = true;
    const auto chunk = chunk_;
    const auto options = velocityOptions_;
    const QPointer<RadarMapCanvas> guard(this);
    QThreadPool::globalInstance()->start([guard, generation, chunk, options] {
        const auto processed = sharedProcessedVelocity(chunk, options);
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, generation, processed] {
            if (!guard || generation != guard->velocityGeneration_) return;
            guard->processedVelocity_ = processed;
            guard->velocityBuildPending_ = false;
            guard->invalidateOverlay();
            guard->update();
        }, Qt::QueuedConnection);
    });
}

void RadarMapCanvas::rebuildProbeIndex() {
    probeRayIndex_.clear();
    if (!chunk_) return;
    probeRayIndex_.reserve(static_cast<qsizetype>(chunk_->rays.size()));
    for (std::size_t rayIndex = 0; rayIndex < chunk_->rays.size(); ++rayIndex) {
        const auto& ray = chunk_->rays[rayIndex];
        if (elevationFilter_ && ray.elevationNumber != *elevationFilter_) continue;
        if (!ray.has(product_)) continue;
        auto azimuth = std::fmod(static_cast<double>(ray.azimuthDegrees), 360.0);
        if (azimuth < 0.0) azimuth += 360.0;
        probeRayIndex_.push_back({azimuth, rayIndex});
    }
    std::sort(probeRayIndex_.begin(), probeRayIndex_.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
}

void RadarMapCanvas::setMeasureMode(const bool enabled) {
    measureMode_ = enabled;
    if (enabled) crossSectionMode_ = false;
    if (!enabled) {
        measureStart_.reset();
        measureEnd_.reset();
    }
    setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void RadarMapCanvas::setCrossSectionMode(const bool enabled) {
    crossSectionMode_ = enabled;
    if (enabled) measureMode_ = false;
    if (!enabled) {
        measureStart_.reset();
        measureEnd_.reset();
    }
    setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void RadarMapCanvas::setVolumeQuality(const int decodedChunks, const int totalChunks,
                                      const int failedChunks, const std::size_t rejectedMessages,
                                      const std::size_t rejectedBlocks) {
    qualityText_ = totalChunks > 0
                       ? QStringLiteral("%1/%2 chunks%3")
                             .arg(decodedChunks).arg(totalChunks)
                             .arg(failedChunks > 0 ? QStringLiteral(" | %1 missing").arg(failedChunks)
                                                   : QStringLiteral(" | complete"))
                       : QString{};
    if (rejectedMessages > 0 || rejectedBlocks > 0)
        qualityText_ += QStringLiteral(" | parser rejected %1 message(s), %2 block(s)")
                            .arg(rejectedMessages).arg(rejectedBlocks);
    update();
}

void RadarMapCanvas::refreshScanDiagnostics() {
    scanQualityText_.clear();
    if (!chunk_) return;
    const auto diagnostics = analyzeScan(*chunk_, product_, elevationFilter_);
    if (diagnostics.rayCount == 0) return;
    scanQualityText_ = QStringLiteral("%1 rays | %2%3 coverage | max gap %4%3")
                           .arg(diagnostics.rayCount)
                           .arg(diagnostics.azimuthCoverageDegrees, 0, 'f', 0)
                           .arg(QChar(0x00b0))
                           .arg(diagnostics.largestAzimuthGapDegrees, 0, 'f', 1);
}

void RadarMapCanvas::setLoading(const bool loading, const QString& message) {
    loading_ = loading;
    loadingMessage_ = message;
    if (loading) loadingAngle_ = 0;
    if (loading && !loadingTimer_->isActive()) loadingTimer_->start();
    else if (!loading) loadingTimer_->stop();
    update();
}

void RadarMapCanvas::showNationalView() {
    center_ = {-96.5, 38.5};
    zoom_ = 4;
    invalidateWorldCaches(true);
    invalidateOverlay();
    update();
    emit viewChanged(center_, zoom_);
}

void RadarMapCanvas::recenterSelected() {
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude) return;
    center_ = {*site->longitude, *site->latitude};
    zoom_ = 6;
    invalidateWorldCaches(true);
    invalidateOverlay();
    update();
    emit viewChanged(center_, zoom_);
}

void RadarMapCanvas::zoomIn() {
    setZoomAt(zoom_ + 1, QPointF(width() / 2.0, height() / 2.0));
}

void RadarMapCanvas::zoomOut() {
    setZoomAt(zoom_ - 1, QPointF(width() / 2.0, height() / 2.0));
}

void RadarMapCanvas::setView(const QPointF& center, const int zoom) {
    const auto previousZoom = zoom_;
    center_ = {std::clamp(center.x(), -180.0, 180.0), std::clamp(center.y(), -85.0, 85.0)};
    zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    invalidateWorldCaches(zoom_ != previousZoom);
    if (zoom_ != previousZoom) invalidateOverlay(true);
    update();
}

void RadarMapCanvas::setActivePanel(const bool active) {
    activePanel_ = active;
    update();
}

QPointF RadarMapCanvas::viewCenter() const {
    return center_;
}

int RadarMapCanvas::viewZoom() const {
    return zoom_;
}

QString RadarMapCanvas::radarId() const {
    return selectedRadar_;
}

ProductKind RadarMapCanvas::product() const {
    return product_;
}

std::optional<int> RadarMapCanvas::elevationFilter() const {
    return elevationFilter_;
}

VelocityProcessingOptions RadarMapCanvas::velocityProcessing() const {
    return velocityOptions_;
}

RadarMapCanvas::PaletteKind RadarMapCanvas::palette() const {
    return palette_;
}

QVector<RadarMapCanvas::PaletteStop> RadarMapCanvas::customPalette() const {
    return customPalette_;
}

QColor RadarMapCanvas::colorForValue(const ProductKind product, const double value,
                                     const PaletteKind palette, const int opacityPercent,
                                     const QVector<PaletteStop>& custom) {
    const auto& descriptor = productDescriptor(product);
    const auto normalized = std::clamp((value - descriptor.minimum) /
                                           (descriptor.maximum - descriptor.minimum), 0.0, 1.0);
    return productColor(product, normalized, palette, opacityPercent, custom);
}

QString RadarMapCanvas::probeText() const {
    return probeText_;
}

void RadarMapCanvas::inspectAt(const QPointF& point) {
    updateProbe(point);
}

int RadarMapCanvas::warningCount() const {
    return warnings_.size();
}

int RadarMapCanvas::stormReportCount() const {
    return stormReports_.size();
}

QPointF RadarMapCanvas::worldPoint(const double latitude, const double longitude) const {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto lat = qDegreesToRadians(std::clamp(latitude, -85.0511, 85.0511));
    const auto sinLat = std::sin(lat);
    const auto x = (longitude + 180.0) / 360.0 * world;
    const auto y = (0.5 - std::log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * kPi)) * world;
    return {x, y};
}

QPointF RadarMapCanvas::mapCenterWorld() const {
    if (cachedWorldCenterZoom_ != zoom_ || cachedCenterCoordinate_ != center_) {
        cachedWorldCenter_ = worldPoint(center_.y(), center_.x());
        cachedCenterCoordinate_ = center_;
        cachedWorldCenterZoom_ = zoom_;
    }
    return cachedWorldCenter_;
}

QPointF RadarMapCanvas::screenPointFromWorld(const QPointF& source) const {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = mapCenterWorld();
    auto dx = source.x() - center.x();
    if (dx > world / 2.0) dx -= world;
    if (dx < -world / 2.0) dx += world;
    return {width() / 2.0 + dx, height() / 2.0 + source.y() - center.y()};
}

QPointF RadarMapCanvas::screenPoint(const double latitude, const double longitude) const {
    return screenPointFromWorld(worldPoint(latitude, longitude));
}

QPointF RadarMapCanvas::geographicPoint(const QPointF& screen) const {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = mapCenterWorld();
    auto x = center.x() + screen.x() - width() / 2.0;
    x = std::fmod(x + world, world);
    if (x < 0.0) x += world;
    const auto y = std::clamp(center.y() + screen.y() - height() / 2.0, 0.0, world);
    const auto longitude = x / world * 360.0 - 180.0;
    const auto n = kPi - 2.0 * kPi * y / world;
    return {longitude, qRadiansToDegrees(std::atan(std::sinh(n)))};
}

QPoint RadarMapCanvas::radarScreenPoint(const QString& radarId) const {
    const auto found = siteIndexes_.constFind(radarId);
    if (found == siteIndexes_.cend()) return {};
    ensureSiteWorldPoints();
    const auto index = *found;
    if (index < static_cast<std::size_t>(siteWorldPoints_.size()) && !siteWorldPoints_.at(index).isNull())
        return screenPointFromWorld(siteWorldPoints_.at(index)).toPoint();
    return {};
}

const RadarSite* RadarMapCanvas::selectedSite() const {
    const auto found = siteIndexes_.constFind(selectedRadar_);
    return found == siteIndexes_.cend() || *found >= sites_.size() ? nullptr : &sites_[*found];
}

void RadarMapCanvas::invalidateWorldCaches(const bool zoomChanged) {
    cachedWorldCenterZoom_ = -1;
    if (zoomChanged) {
        siteWorldZoom_ = -1;
        warningPathsZoom_ = -1;
    }
}

void RadarMapCanvas::ensureSiteWorldPoints() const {
    if (siteWorldZoom_ == zoom_ && siteWorldPoints_.size() == static_cast<qsizetype>(sites_.size())) return;
    siteWorldPoints_.clear();
    siteWorldPoints_.reserve(static_cast<qsizetype>(sites_.size()));
    for (const auto& site : sites_) {
        siteWorldPoints_.push_back(site.latitude && site.longitude
                                       ? worldPoint(*site.latitude, *site.longitude)
                                       : QPointF{});
    }
    siteWorldZoom_ = zoom_;
}

void RadarMapCanvas::invalidateOverlay(const bool preserveForZoomTransition) {
    ++overlayGeneration_;
    if (!preserveForZoomTransition) {
        overlayImage_ = {};
        overlayImageLogicalSize_ = {};
        overlayZoom_ = -1;
    }
}

bool RadarMapCanvas::overlayCacheCoversView() const {
    if (overlayImage_.isNull() || overlayZoom_ != zoom_ || overlayImageLogicalSize_.isEmpty()) return false;
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto currentCenter = mapCenterWorld();
    auto dx = currentCenter.x() - overlayImageCenterWorld_.x();
    if (dx > world / 2.0) dx -= world;
    if (dx < -world / 2.0) dx += world;
    const auto dy = currentCenter.y() - overlayImageCenterWorld_.y();
    const auto horizontalMargin = (overlayImageLogicalSize_.width() - width()) / 2.0;
    const auto verticalMargin = (overlayImageLogicalSize_.height() - height()) / 2.0;
    return std::abs(dx) <= std::max(0.0, horizontalMargin - 24.0) &&
           std::abs(dy) <= std::max(0.0, verticalMargin - 24.0);
}

void RadarMapCanvas::rebuildOverlay() {
    if (overlayBuildPending_ || velocityBuildPending_ || width() <= 0 || height() <= 0) return;
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude || !chunk_) {
        overlayZoom_ = zoom_;
        return;
    }
    overlayBuildPending_ = true;
    const auto generation = overlayGeneration_;
    const auto targetZoom = zoom_;
    const auto baseLatitude = *site->latitude;
    const auto baseLongitude = *site->longitude;
    const auto product = product_;
    const auto elevation = elevationFilter_;
    const auto belowThresholdVisible = belowThresholdVisible_;
    const auto palette = palette_;
    const auto customPalette = customPalette_;
    const auto opacity = radarOpacity_;
    const auto chunk = chunk_;
    const auto processedVelocity = processedVelocity_;
    const auto viewportSize = size();
    const auto devicePixelRatio = devicePixelRatioF();
    const auto centerCoordinate = center_;
    const QPointer<RadarMapCanvas> guard(this);
    QThreadPool::globalInstance()->start([
        guard, generation, targetZoom, baseLatitude, baseLongitude, product, elevation,
        belowThresholdVisible, palette, customPalette, opacity, chunk, processedVelocity,
        viewportSize, devicePixelRatio, centerCoordinate
    ] {
        constexpr int kOverlayMargin = 256;
        QHash<QRgb, QPainterPath> paths;
        double maximumRangeMeters = 0.0;
        const auto& descriptor = productDescriptor(product);
        const auto worldPointAt = [targetZoom](const double latitude, const double longitude) {
            const auto world = static_cast<double>(RadarMapCanvas::kTileSize * (1 << targetZoom));
            const auto lat = qDegreesToRadians(std::clamp(latitude, -85.0511, 85.0511));
            const auto sinLat = std::sin(lat);
            return QPointF((longitude + 180.0) / 360.0 * world,
                           (0.5 - std::log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * kPi)) * world);
        };
        for (std::size_t rayIndex = 0; rayIndex < chunk->rays.size(); ++rayIndex) {
            const auto& ray = chunk->rays[rayIndex];
            if (elevation && ray.elevationNumber != *elevation) continue;
            if (!ray.has(product)) continue;
            const auto productIndex = static_cast<std::size_t>(product);
            const auto& values = ray.fields.at(productIndex);
            const auto firstGateMeters = ray.firstGateByProduct.at(productIndex);
            const auto gateSpacingMeters = ray.gateSpacingByProduct.at(productIndex);
            if (gateSpacingMeters == 0) continue;
            maximumRangeMeters = std::max(
                maximumRangeMeters,
                static_cast<double>(firstGateMeters) +
                    static_cast<double>(values.size()) * gateSpacingMeters);
            const auto halfAzimuth = std::clamp(
                static_cast<double>(ray.azimuthResolutionDegrees) * 0.5, 0.1, 0.75);
            const auto halfRange = static_cast<double>(gateSpacingMeters) * 0.5;
            for (std::size_t gate = 0; gate < values.size(); ++gate) {
                std::optional<double> value;
                if (product == ProductKind::Velocity && processedVelocity &&
                    rayIndex < processedVelocity->size() && gate < processedVelocity->at(rayIndex).size() &&
                    std::isfinite(processedVelocity->at(rayIndex)[gate])) {
                    value = processedVelocity->at(rayIndex)[gate];
                } else {
                    value = nativeValue(values[gate], ray.scaleByProduct.at(productIndex),
                                        ray.offsetByProduct.at(productIndex));
                }
                const auto belowThreshold = values[gate] == 0;
                const auto rangeFolded = values[gate] == 1;
                if (!value && !(belowThresholdVisible && (belowThreshold || rangeFolded))) continue;
                const auto normalized = value
                    ? std::clamp((*value - descriptor.minimum) /
                                     (descriptor.maximum - descriptor.minimum), 0.0, 1.0)
                    : 0.0;
                const auto color = value
                    ? productColor(product, std::round(normalized * 63.0) / 63.0, palette, opacity, customPalette)
                    : rangeFolded ? QColor(150, 150, 160, 95) : QColor(45, 70, 78, 55);
                const auto rangeCenter = static_cast<double>(firstGateMeters) +
                                         (static_cast<double>(gate) + 0.5) * gateSpacingMeters;
                const auto lowerRange = std::max(0.0, rangeCenter - halfRange);
                const auto upperRange = rangeCenter + halfRange;
                if (upperRange <= 0.0) continue;
                const auto coordinateAt = [&](const double range, const double azimuth) {
                    const auto beam = standardBeamGeometry(range, ray.elevationDegrees);
                    return destinationPoint({baseLatitude, baseLongitude}, azimuth,
                                            beam.surfaceDistanceMeters);
                };
                const auto lowerAzimuth = static_cast<double>(ray.azimuthDegrees) - halfAzimuth;
                const auto upperAzimuth = static_cast<double>(ray.azimuthDegrees) + halfAzimuth;
                const std::array coordinates{
                    coordinateAt(lowerRange, lowerAzimuth), coordinateAt(upperRange, lowerAzimuth),
                    coordinateAt(upperRange, upperAzimuth), coordinateAt(lowerRange, upperAzimuth),
                };
                QPolygonF polygon;
                polygon.reserve(4);
                for (const auto& coordinate : coordinates)
                    polygon.push_back(worldPointAt(coordinate.latitude, coordinate.longitude));
                paths[color.rgba()].addPolygon(polygon);
            }
        }
        const auto overlayLogicalSize = viewportSize + QSize(kOverlayMargin * 2, kOverlayMargin * 2);
        const auto pixelSize = QSize(
            std::max(1, static_cast<int>(std::ceil(overlayLogicalSize.width() * devicePixelRatio))),
            std::max(1, static_cast<int>(std::ceil(overlayLogicalSize.height() * devicePixelRatio))));
        QImage overlayImage(pixelSize, QImage::Format_ARGB32_Premultiplied);
        overlayImage.setDevicePixelRatio(devicePixelRatio);
        overlayImage.fill(Qt::transparent);
        const auto overlayCenterWorld = worldPointAt(centerCoordinate.y(), centerCoordinate.x());
        QPainter overlayPainter(&overlayImage);
        overlayPainter.setPen(Qt::NoPen);
        overlayPainter.translate(overlayLogicalSize.width() / 2.0 - overlayCenterWorld.x(),
                                 overlayLogicalSize.height() / 2.0 - overlayCenterWorld.y());
        for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
            overlayPainter.setBrush(QColor::fromRgba(it.key()));
            overlayPainter.drawPath(it.value());
        }
        overlayPainter.end();
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, generation, targetZoom,
                                          overlayImage = std::move(overlayImage),
                                          overlayLogicalSize, overlayCenterWorld,
                                          maximumRangeMeters]() mutable {
            if (!guard) return;
            guard->overlayBuildPending_ = false;
            if (generation == guard->overlayGeneration_ && targetZoom == guard->zoom_) {
                guard->overlayImage_ = std::move(overlayImage);
                guard->overlayImageLogicalSize_ = overlayLogicalSize;
                guard->overlayImageCenterWorld_ = overlayCenterWorld;
                guard->maximumRangeMeters_ = maximumRangeMeters > 0.0
                                                  ? maximumRangeMeters
                                                  : 230000.0;
                guard->overlayZoom_ = targetZoom;
            }
            guard->update();
        }, Qt::QueuedConnection);
    });
}

void RadarMapCanvas::paintRadarOverlay(QPainter& painter) {
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude) {
        return;
    }

    if (!overlayCacheCoversView()) rebuildOverlay();
    if (!chunk_) return;
    painter.save();
    if (!overlayImage_.isNull()) {
        const auto world = static_cast<double>(kTileSize * (1 << zoom_));
        const auto currentCenter = mapCenterWorld();
        const auto zoomScale = std::ldexp(1.0, zoom_ - overlayZoom_);
        const auto cachedCenterAtCurrentZoom = overlayImageCenterWorld_ * zoomScale;
        auto dx = cachedCenterAtCurrentZoom.x() - currentCenter.x();
        if (dx > world / 2.0) dx -= world;
        if (dx < -world / 2.0) dx += world;
        const auto dy = cachedCenterAtCurrentZoom.y() - currentCenter.y();
        const QSizeF scaledSize(overlayImageLogicalSize_.width() * zoomScale,
                                overlayImageLogicalSize_.height() * zoomScale);
        const QPointF topLeft(
            std::round(width() / 2.0 - scaledSize.width() / 2.0 + dx),
            std::round(height() / 2.0 - scaledSize.height() / 2.0 + dy));
        painter.drawImage(QRectF(topLeft, scaledSize), overlayImage_);
    }
    painter.restore();

    painter.save();
    painter.setPen(QPen(QColor(255, 255, 255, 150), 1));
    painter.setBrush(Qt::NoBrush);
    const auto center = screenPoint(*site->latitude, *site->longitude);
    const auto maximumRangeMeters = maximumRangeMeters_;
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 7));
    std::vector<double> ringRanges{25000.0, 50000.0, 100000.0, maximumRangeMeters};
    std::sort(ringRanges.begin(), ringRanges.end());
    ringRanges.erase(std::unique(ringRanges.begin(), ringRanges.end(), [](double left, double right) {
        return std::abs(left - right) < 1000.0;
    }), ringRanges.end());
    for (const auto rangeMeters : ringRanges) {
        if (rangeMeters > maximumRangeMeters + 1000.0) continue;
        const auto edgeCoordinate = destinationPoint({*site->latitude, *site->longitude}, 90.0, rangeMeters);
        const auto edge = screenPoint(edgeCoordinate.latitude, edgeCoordinate.longitude);
        const auto radius = std::abs(edge.x() - center.x());
        if (radius < 12.0 || radius > std::max(width(), height()) * 1.5) continue;
        painter.drawEllipse(center, radius, radius);
        painter.drawText(QRectF(edge.x() - 36.0, edge.y() - 15.0, 72.0, 13.0), Qt::AlignCenter,
                         QStringLiteral("%1 km").arg(rangeMeters / 1000.0, 0, 'f', 0));
    }

    painter.restore();
}

void RadarMapCanvas::ensureWeatherPaths() const {
    if (warningPathsZoom_ == zoom_ && warningPathsGeneration_ == weatherGeneration_) return;
    warningPaths_.clear();
    stormReportWorldPoints_.clear();
    stormReportWorldPoints_.reserve(stormReports_.size());
    for (const auto& report : stormReports_)
        stormReportWorldPoints_.push_back(worldPoint(report.coordinate.y(), report.coordinate.x()));
    for (int warningIndex = 0; warningIndex < warnings_.size(); ++warningIndex) {
        const auto& warning = warnings_.at(warningIndex);
        if (!warningFilter_.isEmpty() &&
            !warning.event.contains(warningFilter_, Qt::CaseInsensitive) &&
            !warning.severity.contains(warningFilter_, Qt::CaseInsensitive)) continue;
        QColor color("#f4d35e");
        if (warning.event.contains(QStringLiteral("Tornado"), Qt::CaseInsensitive)) color = QColor("#ff3b4d");
        else if (warning.event.contains(QStringLiteral("Severe Thunderstorm"), Qt::CaseInsensitive)) color = QColor("#ff9f43");
        else if (warning.event.contains(QStringLiteral("Flash Flood"), Qt::CaseInsensitive)) color = QColor("#36d17c");
        for (const auto& geographicPolygon : warning.polygons) {
            QPainterPath path;
            path.setFillRule(Qt::OddEvenFill);
            QPolygonF exterior;
            exterior.reserve(geographicPolygon.exterior.size());
            for (const auto& coordinate : geographicPolygon.exterior)
                exterior.push_back(worldPoint(coordinate.y(), coordinate.x()));
            path.addPolygon(exterior);
            for (const auto& geographicHole : geographicPolygon.holes) {
                QPolygonF hole;
                hole.reserve(geographicHole.size());
                for (const auto& coordinate : geographicHole)
                    hole.push_back(worldPoint(coordinate.y(), coordinate.x()));
                path.addPolygon(hole);
            }
            warningPaths_.push_back({warningIndex, color, std::move(path)});
        }
    }
    warningPathsZoom_ = zoom_;
    warningPathsGeneration_ = weatherGeneration_;
}

void RadarMapCanvas::paintWeatherOverlays(QPainter& painter) {
    ensureWeatherPaths();
    if (warningsVisible_) {
        painter.save();
        const auto center = mapCenterWorld();
        const QRectF visibleWorld(center.x() - width() / 2.0, center.y() - height() / 2.0,
                                  width(), height());
        painter.translate(width() / 2.0 - center.x(), height() / 2.0 - center.y());
        for (const auto& cached : warningPaths_) {
            if (!cached.path.boundingRect().intersects(visibleWorld)) continue;
            painter.setPen(QPen(cached.color, 2.0));
            auto fill = cached.color;
            fill.setAlpha(35);
            painter.setBrush(fill);
            painter.drawPath(cached.path);
        }
        painter.restore();
    }

    if (stormReportsVisible_) {
        painter.setPen(QPen(QColor(245, 248, 249), 1.0));
        for (qsizetype reportIndex = 0; reportIndex < stormReports_.size(); ++reportIndex) {
            const auto& report = stormReports_.at(reportIndex);
            if (!stormReportKindsVisible_.at(static_cast<std::size_t>(report.kind))) continue;
            const auto point = reportIndex < stormReportWorldPoints_.size()
                                   ? screenPointFromWorld(stormReportWorldPoints_.at(reportIndex))
                                   : screenPoint(report.coordinate.y(), report.coordinate.x());
            if (!rect().adjusted(-10, -10, 10, 10).contains(point.toPoint())) continue;
            if (report.kind == StormReportKind::Tornado) {
                painter.setBrush(QColor("#ff3b4d"));
                painter.drawPolygon(QPolygonF{{point.x(), point.y() - 6.0},
                                               {point.x() - 5.5, point.y() + 5.0},
                                               {point.x() + 5.5, point.y() + 5.0}});
            } else if (report.kind == StormReportKind::Hail) {
                painter.setBrush(QColor("#53d769"));
                painter.drawEllipse(point, 5.0, 5.0);
            } else {
                painter.setBrush(QColor("#44a7ff"));
                painter.drawRect(QRectF(point.x() - 4.5, point.y() - 4.5, 9.0, 9.0));
            }
        }
    }
}

void RadarMapCanvas::paintLegend(QPainter& painter) {
    const auto& descriptor = productDescriptor(product_);
    const QRectF panel(12, 12, 258, 50);
    painter.setPen(QPen(QColor(205, 216, 222, 130), 1));
    painter.setBrush(QColor(8, 15, 19, 220));
    painter.drawRoundedRect(panel, 4, 4);
    painter.setPen(QColor(235, 240, 243));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold));
    auto label = QString::fromUtf8(descriptor.label.data());
    if (product_ == ProductKind::Velocity && velocityOptions_.mode == VelocityProcessingMode::Dealiased)
        label = QStringLiteral("Dealiased velocity");
    else if (product_ == ProductKind::Velocity && velocityOptions_.mode == VelocityProcessingMode::StormRelative)
        label = QStringLiteral("Storm-relative velocity");
    painter.drawText(QRectF(20, 17, 242, 14), Qt::AlignLeft,
                     QStringLiteral("%1 (%2)").arg(label, QString::fromUtf8(descriptor.unit.data())));
    const QRectF gradientRect(20, 34, 242, 10);
    QLinearGradient gradient(gradientRect.topLeft(), gradientRect.topRight());
    for (int step = 0; step <= 16; ++step) {
        const auto position = step / 16.0;
        gradient.setColorAt(position, productColor(product_, position, palette_, radarOpacity_, customPalette_));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRect(gradientRect);
    painter.setPen(QColor(205, 215, 220));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 7));
    for (int tick = 0; tick < 5; ++tick) {
        const auto fraction = tick / 4.0;
        const auto value = descriptor.minimum + fraction * (descriptor.maximum - descriptor.minimum);
        const auto x = gradientRect.left() + fraction * gradientRect.width();
        const QRectF labelRect(x - 30.0, 45, 60, 12);
        painter.drawText(labelRect, tick == 0 ? Qt::AlignLeft : tick == 4 ? Qt::AlignRight : Qt::AlignCenter,
                         QString::number(value, 'g', 4));
    }

    if (stormReportsVisible_) {
        const QRectF reportPanel(12, 68, 258, 24);
        painter.setPen(QPen(QColor(205, 216, 222, 100), 1));
        painter.setBrush(QColor(8, 15, 19, 205));
        painter.drawRoundedRect(reportPanel, 4, 4);
        painter.setPen(QColor("#ff3b4d"));
        painter.drawText(QRectF(20, 72, 70, 16), Qt::AlignLeft, QStringLiteral("▲ Tornado"));
        painter.setPen(QColor("#53d769"));
        painter.drawText(QRectF(92, 72, 55, 16), Qt::AlignLeft, QStringLiteral("● Hail"));
        painter.setPen(QColor("#44a7ff"));
        painter.drawText(QRectF(153, 72, 70, 16), Qt::AlignLeft, QStringLiteral("■ Wind"));
    }
}

void RadarMapCanvas::paintProbe(QPainter& painter) {
    if (probeText_.isEmpty()) return;
    if (!pinnedInfo_.isEmpty()) {
        const auto parts = probeText_.split(QLatin1Char('\n'));
        const auto isWarning = parts.size() >= 7;
        const auto title = parts.value(0).trimmed();
        const auto severity = isWarning ? parts.value(1).trimmed() : QString{};
        const auto headline = isWarning ? parts.value(2).trimmed() : QString{};
        const auto bodyStart = isWarning ? 3 : 1;
        const auto body = parts.mid(bodyStart).join(QLatin1Char('\n')).trimmed();

        QColor accent("#80d3ff");
        if (title.contains(QStringLiteral("Tornado"), Qt::CaseInsensitive) ||
            severity.contains(QStringLiteral("Extreme"), Qt::CaseInsensitive)) {
            accent = QColor("#ff4d5e");
        } else if (title.contains(QStringLiteral("Severe Thunderstorm"), Qt::CaseInsensitive)) {
            accent = QColor("#ffad4d");
        } else if (title.contains(QStringLiteral("Flash Flood"), Qt::CaseInsensitive)) {
            accent = QColor("#45d98a");
        } else if (isWarning) {
            accent = QColor("#f4d35e");
        }

        const auto cardWidth = std::min(440.0, std::max(280.0, width() - 24.0));
        const auto maximumHeight = std::max(110.0, std::min(300.0, height() - 24.0));
        const auto headlineHeight = headline.isEmpty() ? 0.0 : 34.0;
        const auto cardHeight = std::min(maximumHeight, isWarning ? 290.0 : 150.0);
        const auto x = std::max(12.0, width() - cardWidth - 12.0);
        const auto y = std::clamp(64.0, 12.0, std::max(12.0, height() - cardHeight - 12.0));
        pinnedInfoCardRect_ = QRectF(x, y, cardWidth, cardHeight);

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 75));
        painter.drawRoundedRect(pinnedInfoCardRect_.translated(3.0, 4.0), 8.0, 8.0);
        painter.setPen(QPen(QColor(188, 202, 210, 150), 1.0));
        painter.setBrush(QColor(8, 15, 20, 246));
        painter.drawRoundedRect(pinnedInfoCardRect_, 8.0, 8.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
        painter.drawRoundedRect(QRectF(x, y, 5.0, cardHeight), 3.0, 3.0);

        painter.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
        painter.setPen(QColor("#f5f8fa"));
        painter.drawText(QRectF(x + 16.0, y + 10.0, cardWidth - 54.0, 21.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(title, Qt::ElideRight,
                                                          static_cast<int>(cardWidth - 54.0)));

        const QRectF closeRect(x + cardWidth - 34.0, y + 7.0, 25.0, 25.0);
        painter.setPen(QColor("#aebbc2"));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 11, QFont::DemiBold));
        painter.drawText(closeRect, Qt::AlignCenter, QStringLiteral("×"));

        auto contentTop = y + 38.0;
        if (isWarning) {
            const auto badgeWidth = std::min(150.0,
                painter.fontMetrics().horizontalAdvance(severity.toUpper()) + 18.0);
            const QRectF badge(x + 16.0, contentTop, badgeWidth, 22.0);
            auto badgeFill = accent;
            badgeFill.setAlpha(45);
            painter.setPen(QPen(accent, 1.0));
            painter.setBrush(badgeFill);
            painter.drawRoundedRect(badge, 5.0, 5.0);
            painter.setPen(accent.lighter(125));
            painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold));
            painter.drawText(badge, Qt::AlignCenter, severity.toUpper());
            contentTop += 29.0;
        }

        if (!headline.isEmpty()) {
            painter.setPen(QColor("#e5edf1"));
            painter.setFont(QFont(QStringLiteral("Segoe UI"), 9, QFont::DemiBold));
            painter.drawText(QRectF(x + 16.0, contentTop, cardWidth - 32.0, headlineHeight),
                             Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, headline);
            contentTop += headlineHeight + 3.0;
        }

        const QRectF bodyRect(x + 16.0, contentTop, cardWidth - 32.0,
                              y + cardHeight - contentTop - 12.0);
        painter.setClipRect(bodyRect);
        painter.setPen(QColor("#cbd6dc"));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 8));
        painter.drawText(bodyRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, body);
        painter.restore();
        return;
    }

    pinnedInfoCardRect_ = {};
    const auto metrics = painter.fontMetrics();
    const auto textWidth = metrics.horizontalAdvance(probeText_);
    auto x = probePosition_.x() + 14.0;
    auto y = probePosition_.y() + 14.0;
    if (x + textWidth + 18.0 > width()) x = probePosition_.x() - textWidth - 18.0;
    if (y + 28.0 > height()) y = probePosition_.y() - 30.0;
    const QRectF box(x, y, textWidth + 12.0, 23.0);
    painter.setPen(QPen(QColor(210, 220, 225), 1));
    painter.setBrush(QColor(5, 11, 14, 235));
    painter.drawRoundedRect(box, 3, 3);
    painter.setPen(QColor(242, 246, 248));
    painter.drawText(box.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, probeText_);
}

void RadarMapCanvas::paintStationCard(QPainter& painter) {
    if (hoverRadarId_.isEmpty()) return;
    const auto it = std::find_if(sites_.cbegin(), sites_.cend(), [this](const RadarSite& site) {
        return QString::fromStdString(site.id) == hoverRadarId_;
    });
    if (it == sites_.cend()) return;
    const auto elevation = QStringLiteral("%1 m").arg(it->antennaElevationMeters, 0, 'f', 0);
    const auto text = QStringLiteral("%1  %2\n%3  |  %4")
                          .arg(hoverRadarId_, QString::fromStdString(it->name),
                               QString::fromStdString(it->owner), elevation);
    const QRectF panel(width() - 330.0, 12.0, 318.0, 45.0);
    painter.setPen(QPen(QColor(128, 211, 255, 170), 1));
    painter.setBrush(QColor(8, 15, 19, 230));
    painter.drawRoundedRect(panel, 4, 4);
    painter.setPen(QColor(238, 244, 247));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold));
    painter.drawText(panel.adjusted(9, 4, -8, -4), Qt::AlignLeft | Qt::AlignVCenter, text);
}

void RadarMapCanvas::paintMeasurement(QPainter& painter) {
    if (!measureStart_.has_value() || !measureEnd_.has_value()) return;
    const auto start = screenPoint(measureStart_->y(), measureStart_->x());
    const auto end = screenPoint(measureEnd_->y(), measureEnd_->x());
    const auto measurement = rangeAndBearing({measureStart_->y(), measureStart_->x()},
                                             {measureEnd_->y(), measureEnd_->x()});
    const auto distanceKm = measurement.distanceMeters / 1000.0;
    const auto bearing = measurement.bearingDegrees;
    painter.setPen(QPen(QColor(255, 230, 120), 2, Qt::DashLine));
    painter.drawLine(start, end);
    painter.setBrush(QColor(255, 230, 120));
    painter.drawEllipse(start, 3, 3);
    painter.drawEllipse(end, 3, 3);
    const auto label = QStringLiteral("%1%2 km  %3%4")
                           .arg(crossSectionMode_ ? QStringLiteral("Cross-section  ") : QString{})
                           .arg(distanceKm, 0, 'f', 1)
                           .arg(bearing, 0, 'f', 1)
                           .arg(QChar(0x00b0));
    const auto midpoint = (start + end) * 0.5;
    const QRectF box(midpoint.x() - 56, midpoint.y() - 22, 112, 20);
    painter.setPen(QColor(245, 245, 225));
    painter.setBrush(QColor(8, 15, 19, 225));
    painter.drawRoundedRect(box, 3, 3);
    painter.drawText(box, Qt::AlignCenter, label);
}

QString RadarMapCanvas::tileKey(const int x, const int y) const {
    return QStringLiteral("%1/%2/%3").arg(zoom_).arg(x).arg(y);
}

void RadarMapCanvas::requestVisibleTiles() {
    auto& store = tileStore();
    const auto center = worldPoint(center_.y(), center_.x());
    const auto left = center.x() - width() / 2.0;
    const auto top = center.y() - height() / 2.0;
    const auto firstX = static_cast<int>(std::floor(left / kTileSize));
    const auto lastX = static_cast<int>(std::floor((left + width()) / kTileSize));
    const auto firstY = std::max(0, static_cast<int>(std::floor(top / kTileSize)));
    const auto lastY = std::min((1 << zoom_) - 1,
                                static_cast<int>(std::floor((top + height()) / kTileSize)));
    const auto tileCount = 1 << zoom_;

    for (int tileY = firstY; tileY <= lastY; ++tileY) {
        for (int tileX = firstX; tileX <= lastX; ++tileX) {
            const auto wrappedX = wrappedTileX(tileX, tileCount);
            const auto key = tileKey(wrappedX, tileY);
            if (store.tiles.contains(key)) continue;
            watchTile(store, key, this);
            if (store.pendingKeys.contains(key)) continue;
            const auto url = QUrl(QStringLiteral("https://tile.openstreetmap.org/%1/%2/%3.png")
                                      .arg(zoom_).arg(wrappedX).arg(tileY));
            QNetworkRequest request(url);
            const auto contact = QSettings().value(QStringLiteral("network/contact")).toString().trimmed();
            request.setHeader(QNetworkRequest::UserAgentHeader,
                              contact.isEmpty() ? QStringLiteral("CappedWeather/0.3 operational radar viewer")
                                                : QStringLiteral("CappedWeather/0.3 operational radar viewer (%1)")
                                                      .arg(contact));
            request.setTransferTimeout(std::clamp(QSettings().value(QStringLiteral("network/timeoutSeconds"), 30).toInt(),
                                                  5, 120) * 1000);
            store.pendingKeys.insert(key);
            auto* reply = store.network->get(request);
            store.replies.insert(reply, key);
        }
    }
}

void RadarMapCanvas::panBy(const QPointF& delta) {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = worldPoint(center_.y(), center_.x()) - delta;
    const auto wrappedX = std::fmod(center.x() + world, world);
    const auto x = wrappedX < 0.0 ? wrappedX + world : wrappedX;
    const auto normalizedY = std::clamp(center.y(), 0.0, world);
    const auto longitude = x / world * 360.0 - 180.0;
    const auto n = kPi - 2.0 * kPi * normalizedY / world;
    const auto latitude = qRadiansToDegrees(std::atan(std::sinh(n)));
    center_ = {longitude, latitude};
    invalidateWorldCaches(false);
    update();
    emit viewChanged(center_, zoom_);
}

QString RadarMapCanvas::radarAt(const QPointF& point) const {
    ensureSiteWorldPoints();
    for (std::size_t siteIndex = 0; siteIndex < sites_.size(); ++siteIndex) {
        const auto& site = sites_[siteIndex];
        if (!site.latitude || !site.longitude) continue;
        const auto marker = siteIndex < static_cast<std::size_t>(siteWorldPoints_.size())
                                ? screenPointFromWorld(siteWorldPoints_.at(static_cast<qsizetype>(siteIndex)))
                                : screenPoint(*site.latitude, *site.longitude);
        const auto id = QString::fromStdString(site.id);
        const auto compact = zoom_ <= 4 && id != selectedRadar_;
        const QRectF pill = compact ? QRectF(marker.x() - 7.0, marker.y() - 7.0, 14.0, 14.0)
                                    : QRectF(marker.x() - 24.0, marker.y() - 11.0, 48.0, 22.0);
        if (pill.contains(point)) return QString::fromStdString(site.id);
    }
    return {};
}

QString RadarMapCanvas::overlayAt(const QPointF& point) const {
    ensureWeatherPaths();
    if (stormReportsVisible_) {
        for (qsizetype reportIndex = 0; reportIndex < stormReports_.size(); ++reportIndex) {
            const auto& report = stormReports_.at(reportIndex);
            if (!stormReportKindsVisible_.at(static_cast<std::size_t>(report.kind))) continue;
            const auto reportPoint = reportIndex < stormReportWorldPoints_.size()
                                         ? screenPointFromWorld(stormReportWorldPoints_.at(reportIndex))
                                         : screenPoint(report.coordinate.y(), report.coordinate.x());
            if (QLineF(reportPoint, point).length() > 9.0) continue;
            const auto kind = report.kind == StormReportKind::Tornado ? QStringLiteral("Tornado")
                              : report.kind == StormReportKind::Hail ? QStringLiteral("Hail")
                                                                    : QStringLiteral("Wind");
            const auto timestamp = report.occurredAt.isValid()
                                       ? report.occurredAt.toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"))
                                       : report.time + QStringLiteral(" UTC");
            const auto ageMinutes = report.occurredAt.isValid()
                                        ? report.occurredAt.secsTo(QDateTime::currentDateTimeUtc()) / 60
                                        : -1;
            const auto age = ageMinutes >= 0 ? QStringLiteral(" | %1m old").arg(ageMinutes) : QString{};
            return QStringLiteral("%1 report | %2%3 | %4 %5 | %6, %7\n%8")
                .arg(kind, timestamp, age, report.time, report.magnitude,
                     report.location, report.state, report.comments);
        }
    }
    if (warningsVisible_) {
        const auto center = mapCenterWorld();
        auto worldPointAtCursor = QPointF(center.x() + point.x() - width() / 2.0,
                                          center.y() + point.y() - height() / 2.0);
        const auto world = static_cast<double>(kTileSize * (1 << zoom_));
        worldPointAtCursor.setX(std::fmod(worldPointAtCursor.x() + world, world));
        for (const auto& cached : warningPaths_) {
            if (!cached.path.boundingRect().contains(worldPointAtCursor) ||
                !cached.path.contains(worldPointAtCursor)) continue;
            if (cached.warningIndex >= 0 && cached.warningIndex < warnings_.size()) {
                    const auto& warning = warnings_.at(cached.warningIndex);
                    const auto onset = warning.onset.isValid()
                                           ? warning.onset.toLocalTime().toString(QStringLiteral("MMM d, HH:mm"))
                                           : QStringLiteral("unknown");
                    const auto expires = warning.expires.isValid()
                                             ? warning.expires.toLocalTime().toString(QStringLiteral("MMM d, HH:mm"))
                                             : QStringLiteral("unknown");
                    return QStringLiteral("%1\n%2\n%3\nValid (local): %4 – %5\nSource: %6\n\nDetails: %7\n\nInstructions: %8")
                        .arg(warning.event, warning.severity, warning.headline, onset, expires,
                             warning.senderName, warning.description, warning.instruction);
            }
        }
    }
    return {};
}

void RadarMapCanvas::updateProbe(const QPointF& point) {
    probePosition_ = point;
    if (!pinnedInfo_.isEmpty()) {
        probeText_ = pinnedInfo_;
        update();
        return;
    }
    probeText_.clear();
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude || !chunk_ || chunk_->rays.empty()) {
        update();
        return;
    }

    const auto geographic = geographicPoint(point);
    const auto location = rangeAndBearing({*site->latitude, *site->longitude},
                                          {geographic.y(), geographic.x()});
    const auto groundRangeMeters = location.distanceMeters;
    const auto azimuth = location.bearingDegrees;

    if (probeRayIndex_.empty()) {
        update();
        return;
    }
    const auto candidate = std::lower_bound(
        probeRayIndex_.cbegin(), probeRayIndex_.cend(), azimuth,
        [](const auto& indexedRay, const double target) { return indexedRay.first < target; });
    const auto circularDifference = [azimuth](const double candidateAzimuth) {
        const auto direct = std::abs(candidateAzimuth - azimuth);
        return std::min(direct, 360.0 - direct);
    };
    auto nearest = candidate == probeRayIndex_.cend() ? probeRayIndex_.cbegin() : candidate;
    auto nearestDifference = circularDifference(nearest->first);
    const auto previous = candidate == probeRayIndex_.cbegin()
                              ? std::prev(probeRayIndex_.cend())
                              : std::prev(candidate);
    const auto previousDifference = circularDifference(previous->first);
    if (previousDifference < nearestDifference) {
        nearest = previous;
        nearestDifference = previousDifference;
    }
    const auto nearestRayIndex = nearest->second;
    const auto* nearestRay = &chunk_->rays.at(nearestRayIndex);
    if (!nearestRay || nearestDifference > std::max(1.5, nearestRay->azimuthResolutionDegrees * 1.5)) {
        update();
        return;
    }

    const auto index = static_cast<std::size_t>(product_);
    const auto spacing = nearestRay->gateSpacingByProduct.at(index);
    if (spacing == 0) {
        update();
        return;
    }
    const auto firstGate = nearestRay->firstGateByProduct.at(index);
    const auto slantRangeMeters = slantRangeForSurfaceDistance(groundRangeMeters,
                                                               nearestRay->elevationDegrees);
    const auto gatePosition = (slantRangeMeters - firstGate) / spacing - 0.5;
    const auto gate = static_cast<long long>(std::llround(gatePosition));
    const auto& values = nearestRay->fields.at(index);
    if (gate < 0 || static_cast<std::size_t>(gate) >= values.size()) {
        update();
        return;
    }

    const auto encoded = values.at(static_cast<std::size_t>(gate));
    QString valueText;
    if (encoded == 0) {
        valueText = QStringLiteral("Below threshold");
    } else if (encoded == 1) {
        valueText = QStringLiteral("Range folded");
    } else {
        std::optional<double> value;
        if (product_ == ProductKind::Velocity && processedVelocity_ &&
            nearestRayIndex < processedVelocity_->size() &&
            static_cast<std::size_t>(gate) < processedVelocity_->at(nearestRayIndex).size() &&
            std::isfinite(processedVelocity_->at(nearestRayIndex)[static_cast<std::size_t>(gate)])) {
            value = processedVelocity_->at(nearestRayIndex)[static_cast<std::size_t>(gate)];
        } else {
            value = nativeValue(encoded, nearestRay->scaleByProduct.at(index),
                                nearestRay->offsetByProduct.at(index));
        }
        if (value.has_value()) {
        const auto unit = QString::fromUtf8(productDescriptor(product_).unit.data());
        valueText = QStringLiteral("%1%2%3")
                        .arg(*value, 0, 'f', product_ == ProductKind::CorrelationCoefficient ? 3 : 1)
                        .arg(unit.isEmpty() ? QString{} : QStringLiteral(" "))
                        .arg(unit);
        } else {
            valueText = QStringLiteral("No data");
        }
    }
    const auto beam = standardBeamGeometry(slantRangeMeters, nearestRay->elevationDegrees);
    const auto beamMsl = beam.heightAboveRadarMeters + site->antennaElevationMeters;
    probeText_ = QStringLiteral("%1 | %2 km | %3%4 | tilt %5%4 | beam %6 km MSL")
                     .arg(valueText)
                     .arg(groundRangeMeters / 1000.0, 0, 'f', 1)
                     .arg(azimuth, 0, 'f', 1)
                     .arg(QChar(0x00b0))
                     .arg(nearestRay->elevationDegrees, 0, 'f', 1)
                     .arg(beamMsl / 1000.0, 0, 'f', 2);
    update();
}

void RadarMapCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(18, 24, 28));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = worldPoint(center_.y(), center_.x());
    const auto left = center.x() - width() / 2.0;
    const auto top = center.y() - height() / 2.0;
    const auto firstX = static_cast<int>(std::floor(left / kTileSize));
    const auto lastX = static_cast<int>(std::floor((left + width()) / kTileSize));
    const auto firstY = std::max(0, static_cast<int>(std::floor(top / kTileSize)));
    const auto lastY = std::min((1 << zoom_) - 1,
                                static_cast<int>(std::floor((top + height()) / kTileSize)));
    const auto tileCount = 1 << zoom_;
    const auto& store = tileStore();
    for (int tileY = firstY; tileY <= lastY; ++tileY) {
        for (int tileX = firstX; tileX <= lastX; ++tileX) {
            const auto key = tileKey(wrappedTileX(tileX, tileCount), tileY);
            const QRectF target(tileX * kTileSize - left, tileY * kTileSize - top, kTileSize, kTileSize);
            if (const auto* tile = store.tiles.object(key)) {
                painter.drawImage(target, *tile);
            } else {
                painter.fillRect(target, QColor(28, 36, 41));
                painter.setPen(QColor(47, 59, 66));
                painter.drawRect(target);
            }
        }
    }
    requestVisibleTiles();
    paintRadarOverlay(painter);
    paintWeatherOverlays(painter);

    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold));
    QSet<QString> occupiedMarkerCells;
    ensureSiteWorldPoints();
    for (std::size_t siteIndex = 0; siteIndex < sites_.size(); ++siteIndex) {
        const auto& site = sites_[siteIndex];
        if (!site.latitude || !site.longitude) continue;
        const auto point = siteIndex < static_cast<std::size_t>(siteWorldPoints_.size())
                               ? screenPointFromWorld(siteWorldPoints_.at(static_cast<qsizetype>(siteIndex)))
                               : screenPoint(*site.latitude, *site.longitude);
        if (!rect().adjusted(-50, -30, 50, 30).contains(point.toPoint())) continue;
        const auto id = QString::fromStdString(site.id);
        const auto isTdwr = site.owner == "TDWR";
        const auto selected = id == selectedRadar_;
        if (zoom_ <= 4 && !selected) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(isTdwr ? QColor(188, 105, 223, 205) : QColor(40, 71, 82, 210));
            painter.drawEllipse(point, hoverRadarId_ == id ? 5.0 : 3.0, hoverRadarId_ == id ? 5.0 : 3.0);
            continue;
        }
        if (zoom_ == 5 && !selected && hoverRadarId_ != id) {
            const auto cell = QStringLiteral("%1/%2")
                                  .arg(static_cast<int>(point.x()) / 54)
                                  .arg(static_cast<int>(point.y()) / 25);
            if (occupiedMarkerCells.contains(cell)) continue;
            occupiedMarkerCells.insert(cell);
        }
        const QRectF pill(point.x() - 24.0, point.y() - 11.0, 48.0, 22.0);
        painter.setPen(QPen(selected ? QColor(128, 211, 255)
                                                 : isTdwr ? QColor(223, 177, 255)
                                                          : QColor(210, 220, 224),
                            selected ? 2.0 : 1.0));
        painter.setBrush(selected ? QColor(16, 74, 101, 235)
                                               : isTdwr ? QColor(68, 32, 82, 225)
                                                        : QColor(15, 27, 34, 225));
        painter.drawRoundedRect(pill, 5, 5);
        painter.setPen(selected ? QColor(235, 248, 255) : QColor(230, 235, 238));
        painter.drawText(pill, Qt::AlignCenter, id);
    }

    if (chunk_ && !chunk_->rays.empty()) {
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 9));
        painter.setPen(QColor(232, 238, 240));
        painter.setBrush(QColor(8, 15, 19, 205));
        painter.drawRoundedRect(QRectF(12, height() - 30, 510, 22), 4, 4);
        const auto dataDate = dataTime_.isValid()
                                  ? dataTime_.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss t"))
                                  : QStringLiteral("time unavailable");
        const auto overlayLabel = QStringLiteral("%1 real rays  |  NOAA data: %2%3")
                                      .arg(chunk_->rays.size()).arg(dataDate)
                                      .arg(qualityText_.isEmpty() && scanQualityText_.isEmpty()
                                               ? QString{}
                                               : QStringLiteral("  |  %1%2%3")
                                                     .arg(qualityText_)
                                                     .arg(!qualityText_.isEmpty() && !scanQualityText_.isEmpty()
                                                              ? QStringLiteral(" | ") : QString{})
                                                     .arg(scanQualityText_));
        painter.drawText(QRectF(20, height() - 27, 494, 16), Qt::AlignLeft, overlayLabel);
    }
    painter.setPen(QColor(35, 45, 50));
    painter.setBrush(QColor(245, 248, 249, 225));
    painter.drawRoundedRect(QRectF(width() - 250, height() - 25, 238, 18), 3, 3);
    painter.setPen(QColor(30, 38, 42));
    painter.drawText(QRectF(width() - 242, height() - 22, 222, 13), Qt::AlignRight,
                     QStringLiteral("(c) OpenStreetMap contributors"));
    paintLegend(painter);
    paintProbe(painter);
    paintMeasurement(painter);
    paintStationCard(painter);

    if (loading_) {
        const QRectF chip(width() - 294.0, 64.0, 282.0, 40.0);
        painter.setPen(QPen(QColor(128, 211, 255, 170), 1));
        painter.setBrush(QColor(5, 12, 16, 232));
        painter.drawRoundedRect(chip, 5, 5);
        const QPointF center(chip.left() + 21.0, chip.center().y());
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(128, 211, 255), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(center.x() - 10, center.y() - 10, 20, 20),
                        loadingAngle_ * 16, 250 * 16);
        painter.setPen(QColor(235, 243, 247));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold));
        painter.drawText(chip.adjusted(42, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         loadingMessage_.isEmpty() ? QStringLiteral("Loading radar data")
                                                   : loadingMessage_);
    }
    if (activePanel_) {
        painter.setPen(QPen(QColor(128, 211, 255, 220), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
}

void RadarMapCanvas::mousePressEvent(QMouseEvent* event) {
    emit activated();
    if (event->button() == Qt::LeftButton) {
        if (measureMode_ || crossSectionMode_) {
            measureStart_ = geographicPoint(event->position());
            measureEnd_ = measureStart_;
            event->accept();
            update();
            return;
        }
        dragging_ = true;
        dragged_ = false;
        lastMousePosition_ = event->pos();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void RadarMapCanvas::mouseMoveEvent(QMouseEvent* event) {
    if ((measureMode_ || crossSectionMode_) && measureStart_.has_value()) {
        measureEnd_ = geographicPoint(event->position());
        update();
        event->accept();
        return;
    }
    if (dragging_) {
        const auto delta = event->pos() - lastMousePosition_;
        if (!delta.isNull()) {
            dragged_ = true;
            panBy(delta);
            lastMousePosition_ = event->pos();
        }
        event->accept();
        return;
    }
    pendingProbePosition_ = event->position();
    if (!probeTimer_->isActive()) probeTimer_->start();
    event->accept();
    return;
}

void RadarMapCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && (measureMode_ || crossSectionMode_) && measureStart_.has_value()) {
        measureEnd_ = geographicPoint(event->position());
        if (crossSectionMode_) emit crossSectionRequested(*measureStart_, *measureEnd_);
        update();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        if (!dragged_) {
            if (!pinnedInfo_.isEmpty() && pinnedInfoCardRect_.contains(event->position())) {
                const QRectF closeRect(pinnedInfoCardRect_.right() - 34.0,
                                       pinnedInfoCardRect_.top() + 7.0, 25.0, 25.0);
                if (closeRect.contains(event->position())) {
                    pinnedInfo_.clear();
                    probeText_.clear();
                    pinnedInfoCardRect_ = {};
                    update();
                    event->accept();
                    return;
                }
            }
            const auto radarId = radarAt(event->pos());
            if (!radarId.isEmpty()) {
                pinnedInfo_.clear();
                emit radarClicked(radarId);
            } else {
                pinnedInfo_ = overlayAt(event->position());
                probePosition_ = event->position();
                probeText_ = pinnedInfo_;
                update();
            }
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void RadarMapCanvas::wheelEvent(QWheelEvent* event) {
    const auto steps = event->angleDelta().y() / 120;
    if (steps == 0) return;
    setZoomAt(zoom_ + steps, event->position());
    updateProbe(event->position());
    event->accept();
}

void RadarMapCanvas::setZoomAt(const int zoom, const QPointF& cursor) {
    const auto oldWorld = static_cast<double>(kTileSize * (1 << zoom_));
    const auto oldCenter = worldPoint(center_.y(), center_.x());
    const auto cursorWorld = oldCenter + cursor - QPointF(width() / 2.0, height() / 2.0);
    zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    const auto newWorld = static_cast<double>(kTileSize * (1 << zoom_));
    const auto factor = newWorld / oldWorld;
    const auto newCenter = cursorWorld * factor - (cursor - QPointF(width() / 2.0, height() / 2.0));
    const auto longitude = newCenter.x() / newWorld * 360.0 - 180.0;
    const auto n = kPi - 2.0 * kPi * newCenter.y() / newWorld;
    center_ = {longitude, qRadiansToDegrees(std::atan(std::sinh(n)))};
    invalidateWorldCaches(true);
    invalidateOverlay(true);
    update();
    emit viewChanged(center_, zoom_);
}

void RadarMapCanvas::leaveEvent(QEvent* event) {
    hoverRadarId_.clear();
    if (pinnedInfo_.isEmpty()) probeText_.clear();
    update();
    QWidget::leaveEvent(event);
}

void RadarMapCanvas::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    rebuildProbeIndex();
    refreshScanDiagnostics();
    if (chunk_ && product_ == ProductKind::Velocity &&
        velocityOptions_.mode != VelocityProcessingMode::Native && !processedVelocity_)
        refreshVelocityProcessing();
    update();
}

} // namespace cappedweather
