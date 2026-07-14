#pragma once

#include "core/radar_catalog.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"
#include "app/weather_overlay_source.hpp"

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QPainterPath>
#include <QPointF>
#include <QWidget>

#include <optional>
#include <array>
#include <vector>

class QMouseEvent;
class QEvent;
class QNetworkReply;
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
    };

    explicit RadarMapCanvas(QWidget* parent = nullptr);

    void setSites(std::vector<RadarSite> sites);
    void setSelectedRadar(const QString& radarId);
    void setProduct(ProductKind product);
    void setElevationFilter(std::optional<int> elevationNumber);
    void setDecodedChunk(DecodedChunk chunk, const QDateTime& dataTime);
    void setWeatherOverlays(QVector<WeatherWarning> warnings, QVector<StormReport> reports);
    void setWarningsVisible(bool visible);
    void setStormReportsVisible(bool visible);
    void setStormReportKindVisible(StormReportKind kind, bool visible);
    void setWarningFilter(const QString& filter);
    void setPalette(PaletteKind palette);
    void setRadarOpacity(int percent);
    void setBelowThresholdVisible(bool visible);
    void setMeasureMode(bool enabled);
    void setVolumeQuality(int decodedChunks, int totalChunks, int failedChunks);
    void setLoading(bool loading, const QString& message = {});
    void showNationalView();
    void recenterSelected();
    void zoomIn();
    void zoomOut();
    QPoint radarScreenPoint(const QString& radarId) const;
    QString radarId() const;
    ProductKind product() const;
    QString probeText() const;
    void inspectAt(const QPointF& point);
    int warningCount() const;
    int stormReportCount() const;

signals:
    void radarClicked(const QString& radarId);

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

    QNetworkAccessManager network_;
    QHash<QString, QImage> tiles_;
    QHash<QNetworkReply*, QString> pendingTiles_;
    std::vector<RadarSite> sites_;
    QString selectedRadar_;
    ProductKind product_ = ProductKind::Reflectivity;
    std::optional<int> elevationFilter_;
    DecodedChunk chunk_;
    QHash<QRgb, QPainterPath> overlayPaths_;
    int overlayZoom_ = -1;
    QVector<WeatherWarning> warnings_;
    QVector<StormReport> stormReports_;
    QDateTime dataTime_;
    QString loadingMessage_;
    QString qualityText_;
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
    int radarOpacity_ = 80;
    bool belowThresholdVisible_ = false;
    bool measureMode_ = false;
    int loadingAngle_ = 0;
    QPointF probePosition_;
    QString probeText_;
    std::optional<QPointF> measureStart_;
    std::optional<QPointF> measureEnd_;
};

} // namespace cappedweather
