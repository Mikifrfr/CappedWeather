#pragma once

#include "core/radar_catalog.hpp"
#include "core/circulation_detector.hpp"
#include "core/bright_band.hpp"
#include "core/debris_detector.hpp"
#include "core/hail_detector.hpp"
#include "core/hail_swath.hpp"
#include "core/tbss_detector.hpp"
#include "core/zdr_column.hpp"
#include "app/spc_convective.hpp"
#include "app/nhc_cyclone.hpp"
#include "app/wpc_ero.hpp"
#include "app/hms_smoke.hpp"
#include "app/dat_paths.hpp"
#include "app/hrrr_source.hpp"
#include "app/goes_source.hpp"
#include "app/goes_geo_cache.hpp"
#include "app/mosaic_source.hpp"
#include "app/metar_source.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "core/scan_diagnostics.hpp"
#include "core/velocity_processing.hpp"
#include "core/lightning_overlay.hpp"
#include "core/radar_lightning_fusion.hpp"
#include "core/storm_tracker.hpp"
#include "app/weather_overlay_source.hpp"
#include "app/placefile.hpp"
#include "core/volume_brick.hpp"

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QUrl>
#include <QWidget>

#include <optional>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <QList>
#include <QMutex>

class QMouseEvent;
class QKeyEvent;
class QEnterEvent;
class QEvent;
class QGestureEvent;
class QNativeGestureEvent;
class QPaintEvent;
class QPainter;
class QShowEvent;
class QResizeEvent;
class QTimer;
class QWheelEvent;
class QContextMenuEvent;

namespace cappedweather {

// Basemap providers. CARTO Dark is the default: free single-layer XYZ tiles
// (labels baked in, standard z/x/y order) that take a per-user API key,
// so a public release never funnels every install through one shared
// keyless endpoint. Esri World Dark Gray stays as a legacy fallback (base
// + reference layers, z/y/x order). Custom covers any keyed XYZ template
// with {z}/{x}/{y}/{s}/{key} placeholders.
enum class BasemapProvider {
    CartoDark = 0,
    CartoLight = 1,
    EsriDark = 2,
    Custom = 3,
};
BasemapProvider basemapProviderFromInt(int value);
QString basemapProviderName(BasemapProvider provider);
// Settings-driven (QSettings basemap/*); what the tile fetcher uses.
BasemapProvider basemapProviderFromSettings();
QString basemapApiKeyFromSettings();
struct BasemapTileUrls {
    QUrl base;
    QUrl reference;  // empty when the provider is single-layer (CARTO/custom)
};
BasemapTileUrls basemapTileUrls(int zoom, int x, int y);
BasemapTileUrls basemapTileUrls(int zoom, int x, int y, BasemapProvider provider,
                                const QString& apiKey, const QString& customTemplate);
// Painted attribution line for the active provider (CARTO terms require the
// OSM + CARTO credit to stay visible).
QString basemapAttribution();
QString basemapAttribution(BasemapProvider provider, const QString& customAttribution);
// One-click custom-template gallery for the Settings dialog. Static presets
// fill the template + attribution fields and switch the provider to Custom;
// the RainViewer mosaic needs one anonymous metadata fetch first (its tile
// path rotates every 5 minutes), resolved by rainviewerTileTemplate().
struct BasemapPreset {
    QString name;
    QString templateUrl;  // {z}/{x}/{y} placeholders, ready for Custom
    QString attribution;
};
QVector<BasemapPreset> basemapPresets();
// Resolves the latest RainViewer past frame from a weather-maps.json payload
// (https://api.rainviewer.com/public/weather-maps.json) into a static Custom
// template. Pure, hermetic-testable; empty template + error on bad input.
QString rainviewerTileTemplate(const QByteArray& weatherMapsJson, QString* error = nullptr);
// Interface text scale multiplier for all canvas HUD painters.
void setHudFontScale(double scale);

// Per-gate echo geometry snapshot: world-space quads + normalized values,
// computed once per (volume, product, elevation, zoom) and reused by every
// pan-triggered re-raster instead of redoing destinationPoint/mercator trig
// per gate per rebuild. float32 world pixels stay sub-pixel exact; only
// gates carrying values (or visible below-threshold/folded flags) are kept.
struct EchoGateGeometry {
    struct Gate {
        float x[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        float y[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        float value01 = 0.0F;
        std::uint8_t kind = 0;  // 0 = value, 1 = range-folded, 2 = below-threshold
    };
    std::shared_ptr<const DecodedChunk> chunk;
    std::shared_ptr<const ProcessedVelocity> velocity;
    ProductKind product = ProductKind::Reflectivity;
    std::optional<int> elevation;
    int zoom = -1;
    bool smooth = false;
    bool belowVisible = false;
    double maximumRangeMeters = 0.0;
    std::vector<Gate> gates;
};

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
    void setTvsVisible(bool visible);
    [[nodiscard]] bool tvsVisible() const { return tvsVisible_; }
    [[nodiscard]] int circulationCount() const;
    void setHailVisible(bool visible);
    [[nodiscard]] bool hailVisible() const { return hailVisible_; }
    void setZdrVisible(bool visible);
    [[nodiscard]] bool zdrVisible() const { return zdrVisible_; }
    [[nodiscard]] int zdrColumnCount() const;
    void setTbssVisible(bool visible);
    [[nodiscard]] bool tbssVisible() const { return tbssVisible_; }
    [[nodiscard]] int tbssCount() const;
    [[nodiscard]] int hailCoreCount() const;
    [[nodiscard]] std::vector<HailSwath> hailSwaths() const;
    void setTdsVisible(bool visible);
    [[nodiscard]] bool tdsVisible() const { return tdsVisible_; }
    [[nodiscard]] int tdsCount() const;
    void setStormTrailVisible(bool visible);
    [[nodiscard]] bool stormTrailVisible() const { return stormTrailVisible_; }
    [[nodiscard]] bool hasStormTracks() const;
    void setDisplaySmoothing(bool enabled);
    [[nodiscard]] bool displaySmoothing() const { return displaySmoothing_; }
    [[nodiscard]] QString scanDiagnosticsText() const;
    // Scan pop-out support: the live volume plus a full-tilt breakdown.
    // Null when no volume is loaded; per-tilt rows skip the panel's tilt
    // filter so the dialog always shows every tilt in the volume.
    [[nodiscard]] std::shared_ptr<const DecodedChunk> decodedChunk() const { return chunk_; }
    [[nodiscard]] std::vector<TiltDiagnostics> tiltDiagnostics() const;
    void setWarningsVisible(bool visible);
    void setStormReportsVisible(bool visible);
    void setStormReportKindVisible(StormReportKind kind, bool visible);
    void setWarningFilter(const QString& filter);
    void setSpcOutlooks(QVector<SpcOutlookFeature> outlooks);
    void setSpcOutlookVisible(bool visible);
    [[nodiscard]] bool spcOutlookVisible() const { return spcOutlookVisible_; }
    [[nodiscard]] int spcOutlookCount() const;
    void setMcds(QVector<SpcMcd> mcds);
    void setMcdVisible(bool visible);
    [[nodiscard]] bool mcdVisible() const { return mcdVisible_; }
    [[nodiscard]] int mcdCount() const;
    void setNhcStorms(QVector<NhcStorm> storms);
    void setNhcVisible(bool visible);
    [[nodiscard]] bool nhcVisible() const { return nhcVisible_; }
    [[nodiscard]] int nhcStormCount() const;
    void setWpcOutlooks(QVector<SpcOutlookFeature> outlooks);
    void setWpcVisible(bool visible);
    [[nodiscard]] bool wpcVisible() const { return wpcVisible_; }
    [[nodiscard]] int wpcOutlookCount() const;
    void setHmsSmoke(QVector<HmsSmokePlume> plumes);
    void setHmsVisible(bool visible);
    [[nodiscard]] bool hmsVisible() const { return hmsVisible_; }
    [[nodiscard]] int hmsSmokeCount() const;
    void setHmsFires(QVector<HmsFirePoint> fires);
    [[nodiscard]] int hmsFireCount() const;
    void setDatPaths(QVector<DatPath> paths);
    void setDatVisible(bool visible);
    [[nodiscard]] bool datVisible() const { return datVisible_; }
    [[nodiscard]] int datPathCount() const;
    void setHrrrFrame(std::optional<HrrrFrame> frame);
    void setHrrrVisible(bool visible);
    [[nodiscard]] bool hrrrVisible() const { return hrrrVisible_; }
    [[nodiscard]] bool hrrrHasFrame() const { return hrrrFrame_.has_value(); }
    [[nodiscard]] long long hrrrValidEpoch() const {
        return hrrrFrame_ ? hrrrFrame_->validEpochSeconds : 0;
    }
    void setGoesFrame(std::optional<GoesFrame> frame);
    void setGoesVisible(bool visible);
    [[nodiscard]] bool goesVisible() const { return goesVisible_; }
    [[nodiscard]] bool goesHasFrame() const { return goesFrame_.has_value(); }
    // Test-only oracle switch (see goesExhaustiveForTest_): full-grid scan
    // with identical sampling, colors and raster semantics.
    void setGoesExhaustiveForTest(bool exhaustive) { goesExhaustiveForTest_ = exhaustive; }
    [[nodiscard]] long long goesValidEpoch() const {
        return goesFrame_ ? goesFrame_->validEpochSeconds : 0;
    }
    // Georeference readiness for tests/bench: true once the shared grid
    // for the current frame is attached (or no frame is set).
    [[nodiscard]] bool goesGeoReady() const;
    void setMosaicFrame(std::optional<MosaicFrame> frame);
    void setMosaicVisible(bool visible);
    [[nodiscard]] bool mosaicVisible() const { return mosaicVisible_; }
    [[nodiscard]] bool mosaicHasFrame() const { return mosaicFrame_.has_value(); }
    [[nodiscard]] long long mosaicValidEpoch() const {
        return mosaicFrame_ ? mosaicFrame_->validEpochSeconds : 0;
    }
    void setSpcFireOutlooks(QVector<SpcOutlookFeature> outlooks);
    void setSpcFireVisible(bool visible);
    [[nodiscard]] bool spcFireVisible() const { return spcFireVisible_; }
    [[nodiscard]] int spcFireCount() const;
    // METAR surface observations: decluttered station dots with temp/dew in
    // the probe and full T/Td/wind/category on click.
    void setMetars(QVector<MetarObs> metars);
    void setMetarVisible(bool visible);
    [[nodiscard]] bool metarVisible() const { return metarVisible_; }
    [[nodiscard]] int metarCount() const;
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
    // Warning inbox support: fly the view to a warning polygon and pin its
    // card (index into the warnings vector, same order MainWindow holds).
    void focusWarning(int warningIndex);
    void setHighlightedWarning(int warningIndex);
    [[nodiscard]] int highlightedWarning() const { return highlightedWarning_; }
    [[nodiscard]] bool pinnedCardContains(const QPointF& screenPos) const;
    void togglePinnedCardExpanded();
    // Cross-panel linked probe: sample this panel's current volume at a
    // geographic position (longitude, latitude) hovered in another panel.
    struct ProbeSample {
        bool valid = false;
        QPointF lonLat;
        QString valueText;
        double groundRangeMeters = 0.0;
        double azimuthDegrees = 0.0;
        double elevationDegrees = 0.0;
        double beamMslMeters = 0.0;
    };
    [[nodiscard]] ProbeSample sampleAtGeo(const QPointF& lonLat) const;
    void setLinkedProbe(const QPointF& lonLat, const QString& text, const QString& sourceLabel);
    void clearLinkedProbe();
    void setBelowThresholdVisible(bool visible);
    void setVelocityProcessing(VelocityProcessingOptions options);
    void setMeasureMode(bool enabled);
    void setCrossSectionMode(bool enabled);
    bool crossSectionMode() const { return crossSectionMode_; }
    // 3D preview: any panel flips between the 2D map and a ray-marched view of
    // its own current volume/product. The GPU (resident D3D11) raymarch is the
    // backend when it starts; the CPU march is the fallback and stays built.
    void setVolume3dMode(bool enabled);
    [[nodiscard]] bool volume3dMode() const { return volume3d_; }
    // The brick the last completed 3D frame marched, and what that frame cost.
    // Read by tools/volume3d_probe so the numbers are machine-readable instead
    // of being read off a screenshot. Null / 0 until the first frame lands.
    [[nodiscard]] std::shared_ptr<const VolumeBrick> volume3dBrick() const { return brick_; }
    [[nodiscard]] double volume3dRenderMilliseconds() const { return volumeRenderMs_; }
    [[nodiscard]] double volume3dBuildMilliseconds() const { return volumeBuildMs_; }
    // --- 3D brick box (ROI) -------------------------------------------------
    // The ENU square the 3D preview builds its brick over. Square because the
    // brick's nx and ny share one half extent (src/core/volume_brick.hpp);
    // only a square box can be built, so the wireframe drawn during a drag is
    // that square, and the box the user drags is the box that gets built.
    // Defaults to the full +-150 km box, which is never empty for a live
    // volume; a reset returns to it. Never persisted: a view action.
    struct Volume3dBox {
        double centerXMeters = 0.0;
        double centerYMeters = 0.0;
        double halfExtentMeters = 150000.0;
        [[nodiscard]] bool fullRange() const;
        [[nodiscard]] double widthMeters() const { return 2.0 * halfExtentMeters; }
    };
    // Smallest box a drag may produce, per side (8 km). A two-pixel drag is
    // clamped here rather than building a degenerate grid.
    static constexpr double kVolume3dMinimumBoxMeters = 8000.0;
    static constexpr double kVolume3dFullRangeHalfMeters = 150000.0;
    [[nodiscard]] Volume3dBox volume3dBox() const { return volumeBox_; }
    // Radius the box is clamped to: the ground range of the farthest decoded
    // gate in this panel's chunk (the radar's own coverage), capped.
    [[nodiscard]] double volume3dCoverageMeters() const;
    // Shrink a requested box to the coverage circle and the minimum size,
    // keeping it square and its bearing. Pure, so tests and the probe check the
    // rule without a widget. A box centred outside coverage is pulled in along
    // its own bearing rather than snapped onto the site.
    [[nodiscard]] static Volume3dBox clampVolume3dBox(Volume3dBox box, double coverageMeters);
    // Apply a box: clamp, refit the camera to it, rebuild the brick, re-render.
    void setVolume3dBox(const Volume3dBox& box);
    // Back to the full-range box and the default full-range camera.
    void resetVolume3dBox();
    // Alt+left-drag entry point: map both corners onto the ground plane with
    // the camera the current frame was marched with, square the box, clamp,
    // rebuild. False when the drag mapped to no ground point at all.
    bool applyVolume3dBoxDrag(const QPoint& start, const QPoint& end);
    [[nodiscard]] bool volume3dBoxDragActive() const { return volumeBoxDragActive_; }
    // The box the in-progress drag would build (painted as the wireframe).
    [[nodiscard]] Volume3dBox volume3dBoxDragPreview() const { return volumeBoxDragPreview_; }
    // Test/probe hooks for the frame's own camera: project ENU metres to panel
    // pixels (false behind the eye) and map a panel pixel back onto the ground
    // plane (false above the horizon, where a pixel has no ground point).
    [[nodiscard]] bool volume3dProjectPoint(double xMeters, double yMeters, double zMeters,
                                            QPointF* out) const;
    [[nodiscard]] bool volume3dGroundPointAt(const QPoint& pixel, double* xMeters,
                                             double* yMeters) const;
    // Which raymarcher draws the 3D view. Auto = GPU when the device starts,
    // CPU when it does not (the reason lands on the chip). There is no menu
    // entry: the 3D toggle picks the backend, and this setter exists for the
    // probe, the tests, and CAPPEDWEATHER_VOLUME_GPU=0.
    enum class VolumeBackend { Auto, Gpu, Cpu };
    void setVolume3dBackend(VolumeBackend backend);
    [[nodiscard]] VolumeBackend volume3dBackend() const { return volumeBackend_; }
    // True when the frame on screen came from the GPU march.
    [[nodiscard]] bool volumeFrameGpu() const { return volumeFrameGpu_; }
    // One line explaining a CPU fallback; empty while the GPU is drawing.
    [[nodiscard]] QString volumeGpuError() const { return volumeGpuError_; }
    // Everything the 3D chip's text is made of. Kept as data (not read from the
    // widget inside the formatter) so the UI suite can pin the honesty rules -
    // a CPU frame never names an API, a GPU frame names the one that drew it,
    // and nothing claims a cost before a frame has measured one - hermetically.
    struct Volume3dChipState {
        QString radarName;
        QString productShortName;
        double renderMs = 0.0;   // cost of the frame on screen; 0 = none yet
        bool frameGpu = false;   // the frame on screen came from the GPU march
        QString gpuBackend;      // RHI API of that frame ("Direct3D 11"); empty on CPU
        bool pendingGpu = true;  // which backend the next frame will be asked of
    };
    [[nodiscard]] static QString volume3dChipLabel(const Volume3dChipState& state);
    // March time of the last completed 3D frame (GPU or CPU), for probes/tests.
    [[nodiscard]] double volumeRenderMs() const { return volumeRenderMs_; }
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
    // Drops cached/composited/in-flight shared tiles so a basemap provider
    // or key change refetches from the new endpoint instead of stale layers.
    static void clearSharedTileCache();
    QPointF viewCenter() const;
    int viewZoom() const;
    // Screen point back to lon/lat under the current view. Public alongside
    // the other view queries: tests pin cursor-anchored zoom with it.
    QPointF geographicPoint(const QPointF& screen) const;
    // Visible geographic bounds for GIS export: current georeferencing,
    // no reprojection. Invalid when the panel has no size yet or the view
    // wraps the antimeridian (callers fail closed instead of writing a
    // flipped box).
    struct GeoBounds {
        double west = 0.0;
        double south = 0.0;
        double east = 0.0;
        double north = 0.0;
        bool valid = false;
    };
    [[nodiscard]] GeoBounds visibleLonLatBounds() const;
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
    // Transfer stops for the 3D march: colour sampled from the same shipped
    // palette path (colorForValue) plus the volume alpha ramp. The CPU march and
    // the RHI palette LUT both build from this, so the three views agree.
    static std::vector<RgbaStop> volumeTransferStops(ProductKind product, PaletteKind palette,
                                                     const QVector<PaletteStop>& custom = {});
    // The CPU march accumulates premultiplied RGB (r += (1-a) * colour * shade
    // * gate), so its image must be declared premultiplied too: Format_ARGB32 is
    // straight alpha and QPainter would multiply by alpha a second time when
    // painting it, dimming the preview below its own transfer function. Exposed
    // so the hermetic test marches into the format the panel really ships.
    static constexpr QImage::Format volumePreviewFormat = QImage::Format_ARGB32_Premultiplied;
    // Packs one accumulated march sample into the byte volumePreviewFormat
    // stores: premultiplied RGB plus coverage, scaled to 8 bits. The panel's
    // march and the hermetic pixel-path test share this, so a drift back to
    // straight alpha fails a test instead of quietly dimming the picture.
    static QRgb volumePreviewPixel(float r, float g, float b, float a);
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
    // Scroll-target plumbing for tests: the animation itself converges on a
    // timer (wall-clock, load-sensitive), but target selection and
    // clamping are synchronous and fully deterministic.
    double pinnedCardScrollTargetForTest() const { return pinnedInfoScrollTarget_; }
    double pinnedCardMaxScrollForTest() const {
        return static_cast<double>(
            std::max(0, pinnedInfoContentHeight_ -
                            static_cast<int>(std::floor(pinnedInfoBodyRect_.height()))));
    }
    void inspectAt(const QPointF& point);
    QPoint lightningCellScreenPoint(int index) const;
    int warningCount() const;
    // Keyboard card access: a pinned card with overflowing content can be
    // scrolled with Up/Down (same target physics as the wheel path).
    [[nodiscard]] bool hasScrollablePinnedCard() const;
    void scrollPinnedCard(int pixels);
    int stormReportCount() const;
    // Legend spec hooks (pure, hermetic-testable): the 258 px stack is
    // skipped on narrow panels, the 5 color-table ticks never overlap, and
    // report symbols key only kinds that are actually drawn (layer visible,
    // kind enabled, and at least one report of that kind loaded).
    static bool legendVisibleForWidth(int width);
    static std::array<QRectF, 5> legendTickRects(const QRectF& gradientRect);
    static std::array<bool, 3> legendReportKeys(bool layerVisible,
                                                const std::array<bool, 3>& kindsVisible,
                                                const std::array<bool, 3>& kindsPresent);
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
    // Emitted when the context menu flips the 3D preview so menus/toolbars
    // can stay in sync with the canvas.
    void volume3dModeToggled(bool enabled);
    // Emitted (debounced, same path as the local probe) when the cursor
    // samples a geographic position; MainWindow fans it out to the other
    // panels for the linked readout. probeHidden clears those readouts.
    void probeMoved(const QPointF& lonLat);
    void probeHidden();
    // Emitted on the GUI thread whenever persisted TVS/meso tracks refresh
    // (2+-volume tracks only: what the map draws). MainWindow feeds the
    // opt-in AlertCenter sink; radarId scopes dedup across panels.
    void circulationsUpdated(const std::vector<CirculationDetection>& detections,
                             const QString& radarId);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class PinnedInfoKind {
        None,
        Warning,
        StormReport,
        Lightning,
        Placefile,
        SpcOutlook,
        SpcMcd,
        NhcStorm,
        HmsSmoke,
        DatSurvey,
        Metar,
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
    void paintWarningHighlight(QPainter& painter);
    void paintLinkedProbe(QPainter& painter);
    void paintStationCard(QPainter& painter);
    void paintMeasurement(QPainter& painter);
    void paintLoadingChip(QPainter& painter);
    // M1 3D preview: CPU raymarch of the panel's VolumeBrick with an orbit
    // camera. Reuses the overlay worker pattern (request ids abort stale
    // renders) so orbiting never stacks up full-frame marches.
    void rebuildVolumeRender();
    void rebuildVolumeRender(bool fast);
    void paintVolume3d(QPainter& painter);
    void paintVolumeGrid(QPainter& painter);
    // 3D box plumbing: the box a drag maps to (clamped), and the camera refit
    // applied when a box lands.
    Volume3dBox volumeBoxFromDrag(const QPoint& start, const QPoint& end, bool* mapped) const;
    void fitVolumeCameraToBox();
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
    bool handleGestureEvent(QGestureEvent* gesture);
    bool handleNativeGestureEvent(QNativeGestureEvent* gesture);
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
    std::atomic<int> overlayRequestId_{0};
    std::shared_ptr<const EchoGateGeometry> echoGeometry_;
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
    // SPC convective context: Day 1/2 categorical outlooks (background fill)
    // and active mesoscale discussions (dashed outlines). Vector-painted per
    // frame like placefiles: a handful of polygons, no raster cache needed.
    QVector<SpcOutlookFeature> spcOutlooks_;
    QVector<SpcMcd> mcds_;
    // SPC fire-weather outlooks Day 1/2 (DryT + WindRH files, same GeoJSON
    // shape, official orange fills). Separate vector so the categorical
    // toggle never hides fire risk and vice versa.
    QVector<SpcOutlookFeature> spcFireOutlooks_;
    void paintSpcOutlook(QPainter& painter);
    void paintSpcFireOutlook(QPainter& painter);
    // NHC cyclones: cone polygons + forecast-track lines, vector-painted
    // like MCDs (a handful of rings, no raster cache).
    QVector<NhcStorm> nhcStorms_;
    void paintNhcOverlays(QPainter& painter);
    // WPC excessive-rainfall outlooks Day 1-3 (own vector, same paint idiom
    // as the SPC outlooks: official fills at low alpha, Day 1 on top).
    QVector<SpcOutlookFeature> wpcOutlooks_;
    void paintWpcOutlook(QPainter& painter);
    // HMS smoke plumes: density-tinted polygons, vector-painted like MCDs.
    QVector<HmsSmokePlume> hmsPlumes_;
    void paintHmsSmoke(QPainter& painter);
    // HMS fire detections: orange dots (decluttered grid like storm reports
    // at low zoom), full satellite/FRP/time on click.
    QVector<HmsFirePoint> hmsFires_;
    void paintHmsFires(QPainter& painter);
    std::optional<HmsFirePoint> hmsFireNear(const QPointF& point) const;
    // DAT survey centerlines: EF-colored polylines, vector-painted.
    QVector<DatPath> datPaths_;
    void paintDatPaths(QPainter& painter);
    // HRRR simulated reflectivity: model-context raster under live echo,
    // viewport-culled per-cell paint. Valid time labels every readout so
    // model output never reads as observed echo.
    std::optional<HrrrFrame> hrrrFrame_;
    void paintHrrr(QPainter& painter);
    // GOES infrared: observed context under live echo, same raster idiom.
    // No click branch for either raster: full-view fields would steal every
    // echo probe, so valid times read on the legend chips and badges.
    std::optional<GoesFrame> goesFrame_;
    // Shared immutable georeference (see GoesGeoCache): one grid per exact
    // grid identity across all canvases, built once off the GUI thread.
    // Null while a build is in flight: paintGoes holds raster (waiting
    // state) until delivery attaches the ready grid.
    std::shared_ptr<const GoesGeoData> goesGeo_;
    void requestGoesGeo();
    void attachGoesGeo(std::shared_ptr<const GoesGeoData> data, GoesGeoIdentity identity);
    // In-flight build waiters per identity (see requestGoesGeo): GUI
    // thread only, so QPointer use is safe by construction.
    using GoesGeoWaiters = QList<QPointer<RadarMapCanvas>>;
    static QMutex& goesGeoPendingMutex();
    static QHash<GoesGeoIdentity, GoesGeoWaiters>& goesGeoPending();
    // Test-only oracle switch: iterate the whole grid with identical
    // sampling, colors and raster semantics (no subwindow, no early-out)
    // so bench/tests can prove the viewport optimization paints the same.
    bool goesExhaustiveForTest_ = false;
    void paintGoes(QPainter& painter);
    // IEM national mosaic: observed context in native NWS colors, same
    // raster idiom and no-click rule as HRRR/GOES.
    std::optional<MosaicFrame> mosaicFrame_;
    void paintMosaic(QPainter& painter);
    void paintMcdOverlays(QPainter& painter);
    QVector<MetarObs> metars_;
    void paintMetars(QPainter& painter);
    // Melting level for the experimental precip-type rings: measured per
    // volume on the fusion worker (same site fix as hail/ZDR). Invalid until
    // the first volume with dual-pol moments completes.
    BrightBandEstimate brightBand_;
    // Nearest METAR within 14 px of a screen point, or nullopt. Shared by
    // the probe suffix and the click-hit branch so hover and pin agree.
    std::optional<MetarObs> metarNear(const QPointF& point) const;
    std::vector<LightningCellSnapshot> lightningCells_;
    std::vector<RadarStormFeature> radarFeatures_;
    std::vector<LightningRadarAssociation> lightningAssociations_;
    // Rotation-watch tier: meso-strength but sub-classification couplet
    // columns (shallow / wide), drawn by paintShearOverlay with a cap and
    // label declutter. Raw per-volume output (no time persistence): the
    // vertical-coherence + reflectivity + unfold-veto gates already reject
    // quiet-echo noise, unlike the old span-heuristic flags.
    std::vector<ShearWatchCandidate> shearWatch_;
    // TVS / mesocyclone markers (GR2-style algorithm layer). Raw per-volume
    // detections are matched across volumes below; only tracks seen in 2+
    // volumes reach circulations_ so single-volume flicker never draws.
    struct PersistedCirculation {
        CirculationDetection detection;
        int hits = 0;
        int misses = 0;
    };
    std::vector<PersistedCirculation> circulationTracks_;
    std::vector<CirculationDetection> circulations_;
    void paintCirculationMarkers(QPainter& painter);
    void persistCirculations(std::vector<CirculationDetection> fresh);
    // VIL-density hail cores (guidance ranges, never diameters). Per-volume
    // output: the VIL field is already volume-smoothed, so no time gate.
    std::vector<HailCore> hailCores_;
    void paintHailCores(QPainter& painter);
    // Hail swaths: per-storm hulls of persisted hail fixes, folded from the
    // same fusion completion as the cores. Drawn under the hail toggle: one
    // layer family, one switch.
    HailSwathTracker hailSwaths_;
    void paintHailSwaths(QPainter& painter);
    std::vector<ZdrColumn> zdrColumns_;
    void paintZdrColumns(QPainter& painter);
    std::vector<TbssDetection> tbssDetections_;
    void paintTbss(QPainter& painter);
    // Debris signatures ride the same 2-hit persistence as circulations: a
    // one-volume CC dip is hail or clutter until it repeats.
    struct PersistedTds {
        TdsDetection detection;
        int hits = 0;
        int misses = 0;
    };
    std::vector<PersistedTds> tdsTracks_;
    std::vector<TdsDetection> tdsDetections_;
    void paintTdsMarkers(QPainter& painter);
    void persistTds(std::vector<TdsDetection> fresh);
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
    bool pinnedInfoExpanded_ = false;
    int highlightedWarning_ = -1;
    // Linked cross-panel readout: a ghost of another panel's cursor.
    QPointF linkedProbeGeo_;
    QString linkedProbeText_;
    QString linkedProbeSource_;
    bool linkedProbeVisible_ = false;
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
    // Touch / trackpad state. Pinch accumulates scale multiplicatively and
    // steps the integer zoom each 1.25x so trackpads and touchscreens share
    // the same cursor-anchored path as the wheel.
    int pinchBaseZoom_ = 0;
    double pinchAccum_ = 1.0;
    // Mouse-wheel/trackpad zoom remainder. High-resolution wheels deliver
    // fractional angleDelta (15/20/40) and precision trackpads deliver
    // pixelDelta-only events, so sub-notch input accumulates here until a
    // whole integer zoom step is owed. Cleared on programmatic view jumps
    // (setView) and whenever the zoom clamps at a limit, so stale fractions
    // can never stick later gestures or make reversing unresponsive.
    // Direction changes simply add (opposite signs cancel); sources mix
    // freely because both normalize to the same 120-angle notch unit.
    double wheelZoomRemainder_ = 0.0;
    // View-history gesture dampers: one undo entry per completed gesture.
    // Drag and touchscreen pinch push lazily on the first applied move or
    // step (a click or a motionless pinch pushes nothing); wheel pushes on
    // the first applied step of each gesture (classic notches stay
    // single-shot, trackpad ScrollBegin/Update share one, momentum after
    // ScrollEnd starts a new one); native pinch has no begin/end, so each
    // applied 1.25x step is its own entry like a wheel notch.
    bool dragPushedHistory_ = false;
    bool pinchPushedHistory_ = false;
    bool wheelGestureActive_ = false;
    // Records the pre-gesture view once per gesture. Discrete input (one
    // notch, one button press) stays single-shot; continuous gestures
    // share the entry opened by their first applied step.
    void noteWheelNavigated(bool discrete);
    bool loading_ = false;
    bool warningsVisible_ = true;
    bool stormReportsVisible_ = true;
    bool spcOutlookVisible_ = true;
    bool spcFireVisible_ = true;
    bool nhcVisible_ = true;
    bool wpcVisible_ = false;
    bool hmsVisible_ = true;
    bool datVisible_ = true;
    bool hrrrVisible_ = false;
    bool goesVisible_ = false;
    bool mosaicVisible_ = false;
    bool mcdVisible_ = true;
    bool metarVisible_ = true;
    bool placefilesVisible_ = true;
    bool lightningVisible_ = true;
    bool lightningFusionVisible_ = true;
    bool shearVisible_ = true;
    bool tvsVisible_ = true;
    bool hailVisible_ = true;
    bool zdrVisible_ = true;
    bool tbssVisible_ = true;
    bool tdsVisible_ = true;
    bool stormTrailVisible_ = true;
    bool displaySmoothing_ = false;
    // One history per storm (matched across volumes by proximity), fed from
    // the fusion completion. Replaces the old single-centroid trail, whose
    // global max jumped between cells.
    StormTracker stormTracker_;
    std::array<bool, 3> stormReportKindsVisible_{true, true, true};
    std::optional<OverlayVisibility> overlayOverrides_;
    std::optional<QPointF> proximityAnchor_; // longitude, latitude
    double proximityRadiusKm_ = 200.0;
    PaletteKind palette_ = PaletteKind::Capped;
    QVector<PaletteStop> customPalette_;
    // M1 3D preview state: orbit camera plus the panel's brick and its last
    // render. Brick rebuilds only when chunk/product change; camera moves
    // re-march the cached brick. Same request-id abort pattern as the echo
    // overlay so fast orbits cancel stale marches instead of queueing them.
    // The image is either the resident GPU raymarch's readback or the CPU
    // march's QImage; the panel paints it the same way and the chip says which.
    bool volume3d_ = false;
    VolumeBackend volumeBackend_ = VolumeBackend::Auto;
    bool volumeFrameGpu_ = false;
    // The RHI API that drew the last published frame ("Direct3D 11"), empty
    // whenever that frame was the CPU march's. Written with volumeFrameGpu_ so
    // the chip can never name an API that did not produce the pixels.
    QString volumeGpuBackend_;
    bool volumeGpuFailed_ = false; // sticky: a dead device is never retried per frame
    QString volumeGpuError_;
    bool volumeDragging_ = false;
    bool volumePanDragging_ = false; // middle-drag moves the focus point
    std::shared_ptr<const VolumeBrick> brick_;
    std::shared_ptr<const DecodedChunk> brickChunk_;
    ProductKind brickProduct_ = ProductKind::Reflectivity;
    QImage volumeImage_;
    QSize volumeImageLogicalSize_;
    double volumeOrbitAzimuth_ = 180.0; // degrees, 180 = looking north from the south
    double volumeOrbitElevation_ = 40.0; // degrees above the horizon
    double volumeDistance_ = 420000.0; // meters, eye distance from brick center
    double volumeTarget_[3] = {0.0, 0.0, 7000.0}; // meters ENU, middle-drag moves it
    // Camera the cached image was marched with; the orientation grid draws
    // with the same values so echo and grid never disagree mid-orbit.
    double volumeImageAzimuth_ = 180.0;
    double volumeImageElevation_ = 40.0;
    double volumeImageDistance_ = 420000.0;
    double volumeImageTarget_[3] = {0.0, 0.0, 7000.0};
    double volumeRenderMs_ = 0.0;
    double volumeBuildMs_ = 0.0; // cost of the last brick build (0 = reused a cached brick)
    Volume3dBox volumeBox_;  // box the next brick build uses, ENU metres
    Volume3dBox brickBox_;   // box the cached brick was actually built over
    bool volumeBoxDragActive_ = false;
    QPoint volumeBoxDragStart_;
    QPoint volumeBoxDragEnd_;
    Volume3dBox volumeBoxDragPreview_;
    bool volumeBoxDragPreviewValid_ = false;
    std::atomic<int> volumeRequestId_{0};
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
