#include "app/cross_section_widget.hpp"

#include "core/product.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace cappedweather {

CrossSectionWidget::CrossSectionWidget(std::shared_ptr<const DecodedChunk> chunk,
                                       const GeographicCoordinate radar,
                                       const double radarElevationMeters, const QPointF start,
                                       const QPointF end, const ProductKind product,
                                       const RadarMapCanvas::PaletteKind palette,
                                       QVector<RadarMapCanvas::PaletteStop> customPalette,
                                       const VelocityProcessingOptions velocityOptions, QWidget* parent)
    : QWidget(parent), chunk_(std::move(chunk)), radar_(radar),
      radarElevationMeters_(radarElevationMeters), start_(start), end_(end),
      product_(product), palette_(palette), customPalette_(std::move(customPalette)),
      velocityOptions_(velocityOptions) {
    setMinimumSize(760, 440);
    buildCells();
}

bool CrossSectionWidget::exportCsv(const QString& path) const {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream stream(&file);
    stream << "distance_km,height_msl_km,value," << QString::fromUtf8(productDescriptor(product_).unit.data()) << '\n';
    for (const auto& cell : cells_)
        stream << cell.distanceMeters / 1000.0 << ',' << cell.heightMslMeters / 1000.0 << ',' << cell.value << '\n';
    return stream.status() == QTextStream::Ok;
}

void CrossSectionWidget::buildCells() {
    cells_.clear();
    if (!chunk_) return;
    const auto transect = rangeAndBearing({start_.y(), start_.x()}, {end_.y(), end_.x()});
    transectLengthMeters_ = transect.distanceMeters;
    if (transectLengthMeters_ < 100.0) return;
    const auto velocity = product_ == ProductKind::Velocity
                              ? processVelocity(*chunk_, velocityOptions_)
                              : std::vector<std::vector<double>>{};
    const auto productIndex = static_cast<std::size_t>(product_);
    constexpr int kSamples = 180;
    for (int sample = 0; sample < kSamples; ++sample) {
        const auto along = transectLengthMeters_ * sample / static_cast<double>(kSamples - 1);
        const auto coordinate = destinationPoint({start_.y(), start_.x()}, transect.bearingDegrees, along);
        const auto fromRadar = rangeAndBearing(radar_, coordinate);
        std::map<int, std::pair<double, std::size_t>> nearestByTilt;
        for (std::size_t rayIndex = 0; rayIndex < chunk_->rays.size(); ++rayIndex) {
            const auto& ray = chunk_->rays[rayIndex];
            if (!ray.has(product_)) continue;
            auto difference = std::abs(static_cast<double>(ray.azimuthDegrees) - fromRadar.bearingDegrees);
            difference = std::min(difference, 360.0 - difference);
            if (difference > std::max(1.5, static_cast<double>(ray.azimuthResolutionDegrees) * 1.5)) continue;
            const auto existing = nearestByTilt.find(ray.elevationNumber);
            if (existing == nearestByTilt.end() || difference < existing->second.first)
                nearestByTilt[ray.elevationNumber] = {difference, rayIndex};
        }
        for (const auto& [tilt, nearest] : nearestByTilt) {
            Q_UNUSED(tilt);
            const auto rayIndex = nearest.second;
            const auto& ray = chunk_->rays[rayIndex];
            const auto spacing = ray.gateSpacingByProduct.at(productIndex);
            if (spacing == 0) continue;
            const auto slantRange = slantRangeForSurfaceDistance(fromRadar.distanceMeters, ray.elevationDegrees);
            const auto gatePosition = (slantRange - ray.firstGateByProduct.at(productIndex)) / spacing - 0.5;
            const auto gate = static_cast<long long>(std::llround(gatePosition));
            const auto& encoded = ray.fields.at(productIndex);
            if (gate < 0 || static_cast<std::size_t>(gate) >= encoded.size() || encoded[gate] <= 1) continue;
            std::optional<double> value;
            if (product_ == ProductKind::Velocity && rayIndex < velocity.size() &&
                static_cast<std::size_t>(gate) < velocity[rayIndex].size() &&
                std::isfinite(velocity[rayIndex][gate])) {
                value = velocity[rayIndex][gate];
            } else {
                value = nativeValue(encoded[gate], ray.scaleByProduct.at(productIndex),
                                    ray.offsetByProduct.at(productIndex));
            }
            if (!value) continue;
            const auto beam = standardBeamGeometry(slantRange, ray.elevationDegrees);
            const auto height = beam.heightAboveRadarMeters + radarElevationMeters_;
            cells_.push_back({along, height, *value});
            maximumHeightMeters_ = std::max(maximumHeightMeters_, height * 1.08);
        }
    }
}

void CrossSectionWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(12, 18, 22));
    const QRectF plot(62, 28, width() - 86.0, height() - 82.0);
    painter.setPen(QColor(95, 112, 121));
    painter.drawRect(plot);
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8));
    for (int tick = 0; tick <= 4; ++tick) {
        const auto fraction = tick / 4.0;
        const auto y = plot.bottom() - fraction * plot.height();
        painter.setPen(QColor(48, 61, 68));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(QColor(190, 202, 208));
        painter.drawText(QRectF(2, y - 8, 54, 16), Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("%1 km").arg(maximumHeightMeters_ * fraction / 1000.0, 0, 'f', 1));
    }
    for (int tick = 0; tick <= 5; ++tick) {
        const auto fraction = tick / 5.0;
        const auto x = plot.left() + fraction * plot.width();
        painter.setPen(QColor(190, 202, 208));
        painter.drawText(QRectF(x - 36, plot.bottom() + 8, 72, 18), Qt::AlignCenter,
                         QStringLiteral("%1 km").arg(transectLengthMeters_ * fraction / 1000.0, 0, 'f', 0));
    }
    painter.setPen(Qt::NoPen);
    const auto cellWidth = std::max(2.0, plot.width() / 180.0 + 0.8);
    for (const auto& cell : cells_) {
        const auto x = plot.left() + cell.distanceMeters / transectLengthMeters_ * plot.width();
        const auto y = plot.bottom() - cell.heightMslMeters / maximumHeightMeters_ * plot.height();
        painter.setBrush(RadarMapCanvas::colorForValue(product_, cell.value, palette_, 95, customPalette_));
        painter.drawRect(QRectF(x - cellWidth / 2.0, y - 3.0, cellWidth, 6.0));
    }
    painter.setPen(QColor(232, 239, 242));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
    const auto& descriptor = productDescriptor(product_);
    painter.drawText(QRectF(plot.left(), 4, plot.width(), 20), Qt::AlignCenter,
                     QStringLiteral("%1 vertical cross-section | %2 samples")
                         .arg(QString::fromUtf8(descriptor.label.data())).arg(cells_.size()));
}

} // namespace cappedweather
