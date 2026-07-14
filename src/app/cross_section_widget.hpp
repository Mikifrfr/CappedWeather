#pragma once

#include "app/radar_map_canvas.hpp"
#include "core/level2_decoder.hpp"
#include "core/radar_geometry.hpp"

#include <QWidget>

#include <memory>
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

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Cell {
        double distanceMeters = 0.0;
        double heightMslMeters = 0.0;
        double value = 0.0;
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
    double transectLengthMeters_ = 0.0;
    double maximumHeightMeters_ = 20000.0;
};

} // namespace cappedweather
