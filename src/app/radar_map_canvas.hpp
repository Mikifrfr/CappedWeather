#pragma once

#include "core/radar_catalog.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "core/velocity_processing.hpp"
#include "core/lightning_overlay.hpp"
#include "core/radar_lightning_fusion.hpp"
#include "app/weather_overlay_source.hpp"
#include "app/placefile.hpp"

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QUrl>
#include <QWidget>

#include <optional>
#include <array>
#include <memory>
#include <utility>
#include <vector>

class QMouseEvent;
class QKeyEvent;
class QEnterEvent;
class QEvent;
class QPaintEvent;
class QPainter;
class QShowEvent;
class QTimer;
class QWheelEvent;
class QContextMenuEvent;

namespace cappedweather {

// Keyless dark basemap: Esri World Dark Gray. The canvas style (base
// geography) and reference style (roads, boundaries, place labels) are
// separate XYZ layers; the tile store composites them into one cached image.
// NOTE: Esri uses z/y/x tile order (y before x), unlike most OSM endpoints.
struct BasemapTileUrls {
    QUrl base;
    QUrl reference;
};
BasemapTileUrls basemapTileUrls(int zoom, int x, int y);

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
    void setPlacefiles(QVector<PlacefileDocument> documents);
    void setPlacefilesVisible(bool visible);
    void setLightningCells(std::vector<LightningCellSnapshot> cells);
    void setLightningVisible(bool visible);
    void setLightningFusionVisible(bool visible);
    void setShearVisible(bool visible);
    [[nodiscard]] bool shearVisible() const { return shearVisible_; }
    void setStormTrailVisible(bool visible);
    [[nodiscard]] bool stormTrailVisible() const { return stormTrailVisible_; }
    void setDisplaySmoothing(bool enabled);
    [[nodiscard]] bool displaySmoothing() const { return displaySmoothing_; }
    [[nodiscard]] QString scanDiagnosticsText() const;
    void setWarningsVisible(bool visible);
    void setStormReportsVisible(bool visible);
    void setStormReportKindVisible(StormReportKind kind, bool visible);
    void setWarningFilter(const QString& filter);
    // Per-panel overlay overrides. When `hasOverrides` is false, the global
    // toggles from MainWindow drive visibility (the default for every panel).
    // When `hasOverrides` is true, the four bools apply to this panel alone.
    struct OverlayVisibility {
        bool warnings = true;
        bool stormReports = true;
        bool lightning = true;
        bool lightningFusion = true;
    };
    void setOverlayOverrides(std::optional<OverlayVisibility> overrides);
    [[nodiscard]] std::optional<OverlayVisibility> overlayOverrides() const { return overlayOverrides_; }
    // Geographic proximity filter. When enabled, only warnings whose exterior
    // ring is within `radiusKm` of the anchor lat/lon are shown. Empty vector
    // (the default) means "show everything". Re-evaluated when the anchor or
    // the radius changes.
    void setProximityFilter(std::optional<QPointF> anchor, double radiusKm);
    [[nodiscard]] bool proximityFilterActive() const { return proximityAnchor_.has_value(); }
    void setPalette(PaletteKind palette);
    void setCustomPalette(QVector<PaletteStop> stops);
    void setRadarOpacity(int percent);
    void setWarningOpacity(int percent);
    void setWarningsOnTop(bool onTop);
    [[nodiscard]] int warningOpacity() const { return warningOpacity_; }
    [[nodiscard]] bool warningsOnTop() const { return warningsOnTop_; }
    void clearPinnedOverlay();
    void setBelowThresholdVisible(bool visible);
    void setVelocityProcessing(VelocityProcessingOptions options);
    void setMeasureMode(bool enabled);
    void setCrossSectionMode(bool enabled);
    bool crossSectionMode() const { return crossSectionMode_; }
    void setVolumeQuality(int decodedChunks, int totalChunks, int failedChunks,
                          std::size_t rejectedMessages = 0, std::size_t rejectedBlocks = 0);
    void setLoading(bool loading, const QString& message = {});
    void showNationalView();
    void recenterSelected();
    void zoomIn();
    void zoomOut();
    void setView(const QPointF& center, int zoom);
    void setActivePanel(bool active);
    // Accent color of the active-panel border. Driven by MainWindow::applyTheme.
    void setActiveBorderColor(const QColor& color);
    // Extra attribution line (e.g. OPERA CC BY 4.0) painted above the
    // basemap credit when a non-default data source feeds this panel.
    void setSupplementalAttribution(const QString& text);
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
    // Whether the pinned card overflows and can actually scroll. The card
    // content height depends on font metrics, so tests must not assume
    // scrolling is possible on every platform.
    bool infoCardScrollable() const;
    // Test hook: explicit enter/leave tracking (QWidget::underMouse does not
    // reflect synthesized hover, so the probe debounce cannot rely on it).
    bool hoverActiveForTest() const { return cursorInside_; }
    void inspectAt(const QPointF& point);
    QPoint lightningCellScreenPoint(int index) const;
    int warningCount() const;
    int stormReportCount() const;
    int placefileElementCount() const;
    bool placefilesVisible() const { return placefilesVisible_; }
    int lightningCellCount() const;
    const std::vector<LightningCellSnapshot>& lightningCells() const { return lightningCells_; }
    int radarFeatureCount() const;
    int lightningFusionCount() const;
    // Pin lock: when true, new overlay clicks are ignored so the active card
    // survives comparison clicks. Empties the card and clears the lock.
    void setPinnedOverlayLocked(bool locked);
    [[nodiscard]] bool pinnedOverlayLocked() const { return pinnedInfoLocked_; }
    // Pan/zoom history. User-driven pan/zoom/setView pushes onto the back
    // stack; undoView/redoView pop between the two stacks. Programmatic
    // restores (undo/redo) are excluded from re-recording.
    void pushViewHistory();
    bool canUndoView() const { return viewBack_.size() > 0; }
    bool canRedoView() const { return viewForward_.size() > 0; }
    int viewBackSizeForUi() const { return viewBack_.size(); }
    int viewForwardSizeForUi() const { return viewForward_.size(); }
    void undoView();
    void redoView();
    void clearViewHistory();

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
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class PinnedInfoKind {
        None,
        Warning,
        StormReport,
        Lightning,
        Placefile,
        // A frozen cursor readout. "Inspect at cursor" used to write the same
        // transient text the hover uses, so the next mouse move silently wiped
        // it and the action looked broken. Pinning it makes it readable, and
        // makes the card body selectable.
        Probe,
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
    void paintPlacefiles(QPainter& painter);
    void paintLightningOverlay(QPainter& painter);
    void paintShearOverlay(QPainter& painter);
    void paintStormTrail(QPainter& painter);
    void paintLegend(QPainter& painter);
    void paintScaleBar(QPainter& painter);
    void paintProbe(QPainter& painter);
    void paintStationCard(QPainter& painter);
    void paintMeasurement(QPainter& painter);
    void paintLoadingChip(QPainter& painter);
    QString elidedStatusText() const;
    void invalidateStatusText();
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
    QVector<PlacefileDocument> placefiles_;
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
    // The bottom-left readout is pure HUD chrome and is recomposed from four
    // inputs; cache it so paintEvent does not rebuild it on every frame.
    mutable QString statusTextCache_;
    mutable bool statusTextCacheValid_ = false;
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
    bool pinnedInfoLocked_ = false;
    QPointF center_{-96.5, 38.5}; // longitude, latitude
    int zoom_ = 4;
    // Pan/zoom undo/redo. Bounded ring of (center, zoom) snapshots.
    struct ViewSnapshot { QPointF center; int zoom = 0; };
    static constexpr int kViewHistoryLimit = 50;
    QVector<ViewSnapshot> viewBack_;
    QVector<ViewSnapshot> viewForward_;
    bool suppressHistory_ = false;
    QPoint lastMousePosition_;
    bool dragging_ = false;
    bool dragged_ = false;
    bool loading_ = false;
    bool warningsVisible_ = true;
    bool stormReportsVisible_ = true;
    bool placefilesVisible_ = true;
    bool lightningVisible_ = true;
    bool lightningFusionVisible_ = true;
    bool shearVisible_ = true;
    bool stormTrailVisible_ = true;
    bool displaySmoothing_ = false;
    struct TrailPoint {
        double latitudeDeg = 0.0;
        double longitudeDeg = 0.0;
        double maxDbz = 0.0;
        QDateTime time;
        bool rotating = false;
    };
    std::vector<TrailPoint> stormTrail_;
    static constexpr std::size_t kStormTrailLimit = 24;
    std::array<bool, 3> stormReportKindsVisible_{true, true, true};
    std::optional<OverlayVisibility> overlayOverrides_;
    std::optional<QPointF> proximityAnchor_; // longitude, latitude
    double proximityRadiusKm_ = 200.0;
    PaletteKind palette_ = PaletteKind::Capped;
    QVector<PaletteStop> customPalette_;
    int radarOpacity_ = 80;
    int warningOpacity_ = 90;
    bool warningsOnTop_ = true;
    bool belowThresholdVisible_ = false;
    bool measureMode_ = false;
    bool crossSectionMode_ = false;
    bool activePanel_ = false;
    QColor activeBorderColor_ = QColor(128, 211, 255);
    QString supplementalAttribution_;
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
    // Tracks enter/leave explicitly: QWidget::underMouse() does not reflect
    // synthesized/test hover, so the probe debounce cannot rely on it.
    bool cursorInside_ = false;
    std::optional<QPointF> measureStart_;
    std::optional<QPointF> measureEnd_;
};

} // namespace cappedweather
