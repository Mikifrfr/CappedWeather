#pragma once

#include "core/radar_catalog.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "core/velocity_processing.hpp"
#include "core/lightning_overlay.hpp"
#include "core/radar_lightning_fusion.hpp"
#include "app/weather_overlay_source.hpp"

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QWidget>

#include <optional>
#include <array>
#include <memory>
#include <utility>
#include <vector>

class QMouseEvent;
class QEvent;
class QPaintEvent;
class QPainter;
class QShowEvent;
class QTimer;
class QWheelEvent;
class QContextMenuEvent;

namespace cappedweather {

class RadarMapCanvas final : public QWidget {
    Q_OBJECT

public:
    enum class PaletteKind {
        Capped,
        Operational,
        ColorSafe,
        Custom,
    };

    struct PaletteStop {
        double value = 0.0;
        QColor color;
    };

    explicit RadarMapCanvas(QWidget* parent = nullptr);
    ~RadarMapCanvas() override;

    void setSites(std::vector<RadarSite> sites);
    void setSelectedRadar(const QString& radarId);
    void setProduct(ProductKind product);
    void setElevationFilter(std::optional<int> elevationNumber);
    void setDecodedChunk(DecodedChunk chunk, const QDateTime& dataTime);
    void setDecodedChunk(std::shared_ptr<const DecodedChunk> chunk, const QDateTime& dataTime);
    void setWeatherOverlays(QVector<WeatherWarning> warnings, QVector<StormReport> reports);
    void setLightningCells(std::vector<LightningCellSnapshot> cells);
    void setLightningVisible(bool visible);
    void setLightningFusionVisible(bool visible);
    void setWarningsVisible(bool visible);
    void setStormReportsVisible(bool visible);
    void setStormReportKindVisible(StormReportKind kind, bool visible);
    void setWarningFilter(const QString& filter);
    void setPalette(PaletteKind palette);
    void setCustomPalette(QVector<PaletteStop> stops);
    void setRadarOpacity(int percent);
    void setBelowThresholdVisible(bool visible);
    void setVelocityProcessing(VelocityProcessingOptions options);
    void setMeasureMode(bool enabled);
    void setCrossSectionMode(bool enabled);
    void setVolumeQuality(int decodedChunks, int totalChunks, int failedChunks,
                          std::size_t rejectedMessages = 0, std::size_t rejectedBlocks = 0);
    void setLoading(bool loading, const QString& message = {});
    void showNationalView();
    void recenterSelected();
    void zoomIn();
    void zoomOut();
    void setView(const QPointF& center, int zoom);
    void setActivePanel(bool active);
    QPointF viewCenter() const;
    int viewZoom() const;
    QPoint radarScreenPoint(const QString& radarId) const;
    QString radarId() const;
    ProductKind product() const;
    std::optional<int> elevationFilter() const;
    VelocityProcessingOptions velocityProcessing() const;
    PaletteKind palette() const;
    QVector<PaletteStop> customPalette() const;
    static QColor colorForValue(ProductKind product, double value, PaletteKind palette,
                                int opacityPercent = 100,
                                const QVector<PaletteStop>& custom = {});
    QString probeText() const;
    QRectF infoCardRect() const { return pinnedInfoCardRect_; }
    double infoCardScrollOffset() const { return pinnedInfoScrollOffset_; }
    void inspectAt(const QPointF& point);
    QPoint lightningCellScreenPoint(int index) const;
    int warningCount() const;
    int stormReportCount() const;
    int lightningCellCount() const;
    const std::vector<LightningCellSnapshot>& lightningCells() const { return lightningCells_; }
    int radarFeatureCount() const;
    int lightningFusionCount() const;

signals:
    void radarClicked(const QString& radarId);
    void activated();
    void viewChanged(const QPointF& center, int zoom);
    void crossSectionRequested(const QPointF& start, const QPointF& end);
    // Emitted when the context menu toggles a tool mode so the toolbar state
    // can stay in sync with the canvas.
    void measureModeToggled(bool enabled);
    void crossSectionModeToggled(bool enabled);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    enum class PinnedInfoKind {
        None,
        Warning,
        StormReport,
        Lightning,
    };

    struct OverlayHit {
        QString text;
        PinnedInfoKind kind = PinnedInfoKind::None;
        std::optional<std::size_t> lightningCellIndex;

        OverlayHit() = default;
        OverlayHit(QString textValue,
                   PinnedInfoKind kindValue = PinnedInfoKind::Warning,
                   std::optional<std::size_t> lightningIndex = std::nullopt)
            : text(std::move(textValue)),
              kind(kindValue),
              lightningCellIndex(lightningIndex) {}
    };

    static constexpr int kTileSize = 256;
    static constexpr int kMinZoom = 3;
    static constexpr int kMaxZoom = 19;

    QPointF worldPoint(double latitude, double longitude) const;
    QPointF mapCenterWorld() const;
    QPointF screenPointFromWorld(const QPointF& world) const;
    QPointF screenPoint(double latitude, double longitude) const;
    QPointF geographicPoint(const QPointF& screen) const;
    void rebuildOverlay();
    void invalidateOverlay(bool preserveForZoomTransition = false);
    void paintRadarOverlay(QPainter& painter);
    void paintWeatherOverlays(QPainter& painter);
    void paintLightningOverlay(QPainter& painter);
    void paintLegend(QPainter& painter);
    void paintProbe(QPainter& painter);
    void paintStationCard(QPainter& painter);
    void paintMeasurement(QPainter& painter);
    void updateProbe(const QPointF& point);
    std::optional<std::size_t> lightningCellAt(const QPointF& point) const;
    QString lightningInfo(std::size_t index) const;
    void setPinnedOverlay(const OverlayHit& hit, const QPointF& point);
    void refreshPinnedLightningInfo();
    const RadarSite* selectedSite() const;
    QString tileKey(int x, int y) const;
    void requestVisibleTiles();
    void panBy(const QPointF& delta);
    QString radarAt(const QPointF& point) const;
    OverlayHit overlayAt(const QPointF& point) const;
    void setZoomAt(int zoom, const QPointF& cursor);
    void refreshScanDiagnostics();
    void refreshVelocityProcessing();
    void rebuildProbeIndex();
    void ensureSiteWorldPoints() const;
    void ensureWeatherPaths() const;
    void invalidateWorldCaches(bool zoomChanged = false);
    bool overlayCacheCoversView() const;
    void rebuildWeatherOverlay();
    bool weatherOverlayCacheCoversView() const;
    void invalidateLightningFusion();
    void rebuildLightningFusion();

    std::vector<RadarSite> sites_;
    QHash<QString, std::size_t> siteIndexes_;
    QString selectedRadar_;
    ProductKind product_ = ProductKind::Reflectivity;
    std::optional<int> elevationFilter_;
    std::shared_ptr<const DecodedChunk> chunk_;
    QImage overlayImage_;
    QPointF overlayImageCenterWorld_;
    QSize overlayImageLogicalSize_;
    int overlayZoom_ = -1;
    quint64 overlayGeneration_ = 0;
    quint64 overlayImageGeneration_ = 0;
    bool overlayBuildPending_ = false;
    double maximumRangeMeters_ = 230000.0;
    QImage weatherOverlayImage_;
    QPointF weatherOverlayImageCenterWorld_;
    QSize weatherOverlayImageLogicalSize_;
    int weatherOverlayZoom_ = -1;
    quint64 weatherOverlayImageGeneration_ = 0;
    bool weatherOverlayBuildPending_ = false;
    QVector<WeatherWarning> warnings_;
    QVector<StormReport> stormReports_;
    std::vector<LightningCellSnapshot> lightningCells_;
    std::vector<RadarStormFeature> radarFeatures_;
    std::vector<LightningRadarAssociation> lightningAssociations_;
    struct CachedWarningPath {
        int warningIndex = -1;
        QColor color;
        QPainterPath path;
        QRectF bounds;
    };
    mutable QVector<CachedWarningPath> warningPaths_;
    mutable QVector<QPointF> stormReportWorldPoints_;
    mutable int warningPathsZoom_ = -1;
    mutable quint64 warningPathsGeneration_ = 0;
    quint64 weatherGeneration_ = 1;
    mutable QVector<QPointF> siteWorldPoints_;
    mutable int siteWorldZoom_ = -1;
    mutable QPointF cachedWorldCenter_;
    mutable QPointF cachedCenterCoordinate_;
    mutable int cachedWorldCenterZoom_ = -1;
    QDateTime dataTime_;
    QString loadingMessage_;
    QString qualityText_;
    QString scanQualityText_;
    QString warningFilter_;
    QString hoverRadarId_;
    QString pinnedInfo_;
    PinnedInfoKind pinnedInfoKind_ = PinnedInfoKind::None;
    std::optional<std::pair<int, int>> pinnedLightningCell_;
    QRectF pinnedInfoCardRect_;
    QRectF pinnedInfoBodyRect_;
    QPointF pinnedInfoCardPosition_{-1.0, -1.0};
    QPointF pinnedInfoDragOffset_;
    double pinnedInfoScrollOffset_ = 0.0;
    double pinnedInfoScrollTarget_ = 0.0;
    int pinnedInfoContentHeight_ = 0;
    bool draggingPinnedInfoCard_ = false;
    QPointF center_{-96.5, 38.5}; // longitude, latitude
    int zoom_ = 4;
    QPoint lastMousePosition_;
    bool dragging_ = false;
    bool dragged_ = false;
    bool loading_ = false;
    bool warningsVisible_ = true;
    bool stormReportsVisible_ = true;
    bool lightningVisible_ = true;
    bool lightningFusionVisible_ = true;
    std::array<bool, 3> stormReportKindsVisible_{true, true, true};
    PaletteKind palette_ = PaletteKind::Capped;
    QVector<PaletteStop> customPalette_;
    int radarOpacity_ = 80;
    bool belowThresholdVisible_ = false;
    bool measureMode_ = false;
    bool crossSectionMode_ = false;
    bool activePanel_ = false;
    VelocityProcessingOptions velocityOptions_;
    std::shared_ptr<const ProcessedVelocity> processedVelocity_;
    quint64 velocityGeneration_ = 0;
    bool velocityBuildPending_ = false;
    quint64 lightningFusionGeneration_ = 1;
    quint64 lightningFusionComputedGeneration_ = 0;
    bool lightningFusionBuildPending_ = false;
    QVector<std::pair<double, std::size_t>> probeRayIndex_;
    int loadingAngle_ = 0;
    QTimer* loadingTimer_ = nullptr;
    QTimer* probeTimer_ = nullptr;
    QTimer* pinnedInfoScrollTimer_ = nullptr;
    QPointF pendingProbePosition_;
    QPointF probePosition_;
    QString probeText_;
    std::optional<QPointF> measureStart_;
    std::optional<QPointF> measureEnd_;
};

} // namespace cappedweather
