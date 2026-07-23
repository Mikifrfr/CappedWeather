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
    for (const auto& cell : cells_) {
        if (!std::isfinite(cell.value)) continue;
        stream << cell.distanceMeters / 1000.0 << ',' << cell.heightMslMeters / 1000.0 << ',' << cell.value << '\n';
    }
    return stream.status() == QTextStream::Ok;
}

void CrossSectionWidget::buildCells() {
    cells_.clear();
    sampleCount_ = 0;
    maximumHeightMeters_ = 0.0;
    if (!chunk_) return;
    const auto transect = rangeAndBearing({start_.y(), start_.x()}, {end_.y(), end_.x()});
    transectLengthMeters_ = transect.distanceMeters;
    if (transectLengthMeters_ < 100.0) return;
    const auto velocity = product_ == ProductKind::Velocity
                              ? processVelocity(*chunk_, velocityOptions_)
                              : ProcessedVelocity{};
    const auto productIndex = static_cast<std::size_t>(product_);
    constexpr int kSamples = 220;
    sampleCount_ = kSamples;
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
            if (gate < 0 || static_cast<std::size_t>(gate) >= encoded.size()) continue;
            const auto beam = standardBeamGeometry(slantRange, ray.elevationDegrees);
            const auto height = beam.heightAboveRadarMeters + radarElevationMeters_;
            std::optional<double> value;
            if (encoded[gate] > 1 && product_ == ProductKind::Velocity && rayIndex < velocity.size() &&
                static_cast<std::size_t>(gate) < velocity[rayIndex].size() &&
                std::isfinite(velocity[rayIndex][gate])) {
                value = velocity[rayIndex][gate];
            } else if (encoded[gate] > 1) {
                value = nativeValue(encoded[gate], ray.scaleByProduct.at(productIndex),
                                    ray.offsetByProduct.at(productIndex));
            }
            cells_.push_back({sample, along, height,
                              value.value_or(std::numeric_limits<double>::quiet_NaN())});
            maximumHeightMeters_ = std::max(maximumHeightMeters_, height);
        }
    }
    maximumHeightMeters_ = maximumHeightMeters_ > 0.0
        ? std::clamp(std::ceil(maximumHeightMeters_ * 1.08 / 5000.0) * 5000.0,
                     5000.0, 30000.0)
        : 20000.0;
}

void CrossSectionWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    QLinearGradient background(rect().topLeft(), rect().bottomLeft());
    background.setColorAt(0.0, QColor(12, 20, 25));
    background.setColorAt(1.0, QColor(7, 13, 17));
    painter.fillRect(rect(), background);
    const QRectF plot(74, 42, width() - 100.0, height() - 106.0);
    painter.fillRect(plot, QColor(5, 14, 19));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8));
    for (int tick = 0; tick <= 4; ++tick) {
        const auto fraction = tick / 4.0;
        const auto y = plot.bottom() - fraction * plot.height();
        painter.setPen(QColor(55, 76, 86, tick == 0 ? 190 : 115));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(QColor(184, 204, 214));
        painter.drawText(QRectF(16, y - 8, 50, 16), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(maximumHeightMeters_ * fraction / 1000.0, 'f', 1));
    }
    for (int tick = 0; tick <= 5; ++tick) {
        const auto fraction = tick / 5.0;
        const auto x = plot.left() + fraction * plot.width();
        painter.setPen(QColor(55, 76, 86, tick == 0 ? 190 : 90));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(QColor(184, 204, 214));
        painter.drawText(QRectF(x - 36, plot.bottom() + 8, 72, 18), Qt::AlignCenter,
                         QString::number(transectLengthMeters_ * fraction / 1000.0, 'f', 0));
    }

    std::vector<std::vector<const Cell*>> columns(static_cast<std::size_t>(std::max(0, sampleCount_)));
    int validCellCount = 0;
    for (const auto& cell : cells_) {
        if (cell.sampleIndex >= 0 && cell.sampleIndex < sampleCount_)
            columns.at(static_cast<std::size_t>(cell.sampleIndex)).push_back(&cell);
        if (std::isfinite(cell.value)) ++validCellCount;
    }
    painter.setPen(Qt::NoPen);
    painter.setRenderHint(QPainter::Antialiasing, false);
    for (auto& column : columns) {
        if (column.empty()) continue;
        std::sort(column.begin(), column.end(), [](const Cell* left, const Cell* right) {
            return left->heightMslMeters < right->heightMslMeters;
        });
        const auto sample = column.front()->sampleIndex;
        const auto denominator = std::max(1, sampleCount_ - 1);
        const auto leftFraction = std::clamp((sample - 0.55) / denominator, 0.0, 1.0);
        const auto rightFraction = std::clamp((sample + 0.55) / denominator, 0.0, 1.0);
        const auto xLeft = plot.left() + leftFraction * plot.width();
        const auto xRight = plot.left() + rightFraction * plot.width();
        for (std::size_t index = 0; index < column.size(); ++index) {
            const auto& cell = *column[index];
            if (!std::isfinite(cell.value)) continue;
            const auto nextHeight = index + 1 < column.size()
                ? column[index + 1]->heightMslMeters
                : cell.heightMslMeters + (index > 0
                    ? cell.heightMslMeters - column[index - 1]->heightMslMeters
                    : 500.0);
            const auto lowerHeight = index > 0
                ? (column[index - 1]->heightMslMeters + cell.heightMslMeters) * 0.5
                : std::max(0.0, radarElevationMeters_);
            const auto upperHeight = std::min(
                maximumHeightMeters_, (cell.heightMslMeters + nextHeight) * 0.5);
            const auto yTop = plot.bottom() - upperHeight / maximumHeightMeters_ * plot.height();
            const auto yBottom = plot.bottom() - lowerHeight / maximumHeightMeters_ * plot.height();
            painter.setBrush(RadarMapCanvas::colorForValue(
                product_, cell.value, palette_, 96, customPalette_));
            painter.drawRect(QRectF(xLeft, yTop, std::max(1.0, xRight - xLeft + 0.8),
                                    std::max(1.0, yBottom - yTop + 0.6)));
        }
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(104, 130, 142), 1.0));
    painter.drawRect(plot);

    painter.setPen(QColor(232, 239, 242));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
    const auto& descriptor = productDescriptor(product_);
    painter.drawText(QRectF(plot.left(), 10, plot.width(), 22), Qt::AlignCenter,
                     QStringLiteral("%1 vertical cross-section | %2 columns | %3 valid bins")
                         .arg(QString::fromUtf8(descriptor.label.data()))
                         .arg(sampleCount_).arg(validCellCount));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8));
    painter.setPen(QColor(137, 161, 173));
    painter.drawText(QRectF(plot.left(), height() - 29.0, plot.width(), 18.0),
                     Qt::AlignCenter, QStringLiteral("Distance along transect (km)"));
    painter.save();
    painter.translate(13.0, plot.center().y());
    painter.rotate(-90.0);
    painter.drawText(QRectF(-plot.height() / 2.0, 0.0, plot.height(), 18.0),
                     Qt::AlignCenter, QStringLiteral("Height MSL (km)"));
    painter.restore();
}

} // namespace cappedweather
