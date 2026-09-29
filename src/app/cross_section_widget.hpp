#pragma once

#include "app/radar_map_canvas.hpp"
#include "core/level2_decoder.hpp"
#include "core/radar_geometry.hpp"

#include <QWidget>

#include <memory>
#include <limits>
#include <vector>

namespace cappedweather {

class CrossSectionWidget final : public QWidget {
public:
    CrossSectionWidget(std::shared_ptr<const DecodedChunk> chunk, GeographicCoordinate radar,
                       double radarElevationMeters, QPointF start, QPointF end,
                       ProductKind product, RadarMapCanvas::PaletteKind palette,
                       QVector<RadarMapCanvas::PaletteStop> customPalette,
                       VelocityProcessingOptions velocityOptions, QWidget* parent = nullptr);

    bool exportCsv(const QString& path) const;

    // Live-update a modeless cross-section when the timeline advances.
    // Rebuilds cells for the new chunk and repaints; keeps geometry/start-end.
    void setData(std::shared_ptr<const DecodedChunk> chunk, ProductKind product,
                 RadarMapCanvas::PaletteKind palette,
                 QVector<RadarMapCanvas::PaletteStop> customPalette,
                 VelocityProcessingOptions velocityOptions);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Cell {
        int sampleIndex = 0;
        double distanceMeters = 0.0;
        double heightMslMeters = 0.0;
        double value = std::numeric_limits<double>::quiet_NaN();
    };

    void buildCells();

    std::shared_ptr<const DecodedChunk> chunk_;
    GeographicCoordinate radar_;
    double radarElevationMeters_ = 0.0;
    QPointF start_;
    QPointF end_;
    ProductKind product_ = ProductKind::Reflectivity;
    RadarMapCanvas::PaletteKind palette_ = RadarMapCanvas::PaletteKind::Operational;
    QVector<RadarMapCanvas::PaletteStop> customPalette_;
    VelocityProcessingOptions velocityOptions_;
    std::vector<Cell> cells_;
    int sampleCount_ = 0;
    double transectLengthMeters_ = 0.0;
    double maximumHeightMeters_ = 20000.0;
};

} // namespace cappedweather
