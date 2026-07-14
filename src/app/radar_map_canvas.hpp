#pragma once

#include "core/radar_catalog.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "core/velocity_processing.hpp"
#include "app/weather_overlay_source.hpp"

#include <QColor>
#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QPainterPath>
#include <QPointF>
#include <QWidget>

#include <optional>
#include <array>
#include <memory>
#include <vector>

class QMouseEvent;
class QEvent;
class QNetworkReply;
class QNetworkDiskCache;
class QPaintEvent;
class QPainter;
class QWheelEvent;

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

    void setSites(std::vector<RadarSite> sites);
    void setSelectedRadar(const QString& radarId);
    void setProduct(ProductKind product);
    void setElevationFilter(std::optional<int> elevationNumber);
    void setDecodedChunk(DecodedChunk chunk, const QDateTime& dataTime);
    void setDecodedChunk(std::shared_ptr<const DecodedChunk> chunk, const QDateTime& dataTime);
    void setWeatherOverlays(QVector<WeatherWarning> warnings, QVector<StormReport> reports);
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
    void inspectAt(const QPointF& point);
    int warningCount() const;
    int stormReportCount() const;

signals:
    void radarClicked(const QString& radarId);
    void activated();
    void viewChanged(const QPointF& center, int zoom);
    void crossSectionRequested(const QPointF& start, const QPointF& end);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;

private slots:
    void tileFinished(QNetworkReply* reply);

private:
    static constexpr int kTileSize = 256;
    static constexpr int kMinZoom = 3;
    static constexpr int kMaxZoom = 19;

    QPointF worldPoint(double latitude, double longitude) const;
    QPointF screenPoint(double latitude, double longitude) const;
    QPointF geographicPoint(const QPointF& screen) const;
    void rebuildOverlay();
    void invalidateOverlay();
    void paintRadarOverlay(QPainter& painter);
    void paintWeatherOverlays(QPainter& painter);
    void paintLegend(QPainter& painter);
    void paintProbe(QPainter& painter);
    void paintStationCard(QPainter& painter);
    void paintMeasurement(QPainter& painter);
    void updateProbe(const QPointF& point);
    const RadarSite* selectedSite() const;
    QString tileKey(int x, int y) const;
    void requestVisibleTiles();
    void panBy(const QPointF& delta);
    QString radarAt(const QPointF& point) const;
    QString overlayAt(const QPointF& point) const;
    void setZoomAt(int zoom, const QPointF& cursor);
    void refreshScanDiagnostics();
    void refreshVelocityProcessing();

    QNetworkAccessManager network_;
    QCache<QString, QImage> tiles_;
    QNetworkDiskCache* tileDiskCache_ = nullptr;
    QHash<QNetworkReply*, QString> pendingTiles_;
    std::vector<RadarSite> sites_;
    QString selectedRadar_;
    ProductKind product_ = ProductKind::Reflectivity;
    std::optional<int> elevationFilter_;
    std::shared_ptr<const DecodedChunk> chunk_;
    QHash<QRgb, QPainterPath> overlayPaths_;
    int overlayZoom_ = -1;
    quint64 overlayGeneration_ = 0;
    bool overlayBuildPending_ = false;
    QVector<WeatherWarning> warnings_;
    QVector<StormReport> stormReports_;
    QDateTime dataTime_;
    QString loadingMessage_;
    QString qualityText_;
    QString scanQualityText_;
    QString warningFilter_;
    QString hoverRadarId_;
    QString pinnedInfo_;
    QPointF center_{-96.5, 38.5}; // longitude, latitude
    int zoom_ = 4;
    QPoint lastMousePosition_;
    bool dragging_ = false;
    bool dragged_ = false;
    bool loading_ = false;
    bool warningsVisible_ = true;
    bool stormReportsVisible_ = true;
    std::array<bool, 3> stormReportKindsVisible_{true, true, true};
    PaletteKind palette_ = PaletteKind::Capped;
    QVector<PaletteStop> customPalette_;
    int radarOpacity_ = 80;
    bool belowThresholdVisible_ = false;
    bool measureMode_ = false;
    bool crossSectionMode_ = false;
    bool activePanel_ = false;
    VelocityProcessingOptions velocityOptions_;
    std::shared_ptr<const std::vector<std::vector<double>>> processedVelocity_;
    int loadingAngle_ = 0;
    QPointF probePosition_;
    QString probeText_;
    std::optional<QPointF> measureStart_;
    std::optional<QPointF> measureEnd_;
};

} // namespace cappedweather
