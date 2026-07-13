#include "app/radar_canvas.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cappedweather {

RadarCanvas::RadarCanvas(QWidget* parent)
    : QWidget(parent) {
    setMinimumSize(360, 360);
    setAutoFillBackground(false);
}

void RadarCanvas::setRadarId(const QString& radarId) {
    radarId_ = radarId;
    chunk_ = {};
    elevationFilter_ = std::nullopt;
    objectKey_.clear();
    objectBytes_ = 0;
    receivedAt_ = {};
    update();
}

void RadarCanvas::setProduct(const ProductKind product) {
    product_ = product;
    update();
}

void RadarCanvas::setElevationFilter(const std::optional<float> elevationDegrees) {
    elevationFilter_ = elevationDegrees;
    update();
}

void RadarCanvas::setStatus(const QString& status) {
    status_ = status;
    update();
}

void RadarCanvas::setRawObjectInfo(const QString& key, const qsizetype bytes, const QDateTime& receivedAt) {
    objectKey_ = key;
    objectBytes_ = bytes;
    receivedAt_ = receivedAt;
    update();
}

void RadarCanvas::setDecodedChunk(DecodedChunk chunk) {
    chunk_ = std::move(chunk);
    update();
}

QString RadarCanvas::radarId() const {
    return radarId_;
}

ProductKind RadarCanvas::product() const {
    return product_;
}

void RadarCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(14, 18, 22));

    const auto margin = 30.0;
    const auto side = std::min(width(), height()) - margin * 2.0;
    const QRectF plot((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    paintRadarFrame(painter, plot);

    painter.setPen(QColor(215, 220, 224));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 10));
    painter.drawText(QRectF(18, 12, width() - 36, 20), Qt::AlignLeft, radarId_);
    painter.setPen(QColor(160, 170, 180));
    painter.drawText(QRectF(18, 32, width() - 36, 20), Qt::AlignLeft,
                     QString::fromUtf8(productDescriptor(product_).label.data()));

    painter.setPen(QColor(190, 198, 204));
    painter.drawText(QRectF(18, height() - 55, width() - 36, 20), Qt::AlignLeft, status_);
    if (!objectKey_.isEmpty()) {
        painter.setPen(QColor(120, 132, 142));
        const auto details = QStringLiteral("Object: %1  |  %2 bytes  |  %3")
                                 .arg(objectKey_)
                                 .arg(objectBytes_)
                                 .arg(receivedAt_.toString(Qt::ISODate));
        painter.drawText(QRectF(18, height() - 35, width() - 36, 20), Qt::AlignLeft, details);
    }
}

void RadarCanvas::paintRadarFrame(QPainter& painter, const QRectF& plot) {
    const auto center = plot.center();
    const auto radius = plot.width() / 2.0;

    painter.setPen(QPen(QColor(54, 66, 74), 1));
    painter.setBrush(Qt::NoBrush);
    for (int ring = 1; ring <= 4; ++ring) {
        const auto ringRadius = radius * static_cast<double>(ring) / 4.0;
        painter.drawEllipse(center, ringRadius, ringRadius);
    }
    painter.drawLine(QPointF(center.x() - radius, center.y()), QPointF(center.x() + radius, center.y()));
    painter.drawLine(QPointF(center.x(), center.y() - radius), QPointF(center.x(), center.y() + radius));

    painter.setPen(QColor(90, 110, 122));
    painter.drawText(QPointF(center.x() + 6, center.y() - radius + 16), QStringLiteral("N"));
    painter.drawText(QPointF(center.x() + radius - 16, center.y() - 6), QStringLiteral("E"));
    painter.drawText(QPointF(center.x() - 6, center.y() + radius - 6), QStringLiteral("S"));
    painter.drawText(QPointF(center.x() - radius + 6, center.y() - 6), QStringLiteral("W"));

    paintEchoes(painter, plot);

    if (chunk_.rays.empty()) {
        painter.setPen(QColor(110, 124, 132));
        painter.drawText(QRectF(center.x() - 130, center.y() - 12, 260, 24), Qt::AlignCenter,
                         QStringLiteral("Waiting for a real NOAA Level II product"));
    }
}

void RadarCanvas::paintEchoes(QPainter& painter, const QRectF& plot) {
    if (chunk_.rays.empty()) {
        return;
    }

    const auto center = plot.center();
    const auto radius = plot.width() / 2.0;
    constexpr double maxRangeMeters = 230000.0;
    const auto& descriptor = productDescriptor(product_);

    for (const auto& ray : chunk_.rays) {
        if (elevationFilter_.has_value() &&
            std::abs(ray.elevationDegrees - *elevationFilter_) > 0.06F) {
            continue;
        }
        if (!ray.has(product_)) {
            continue;
        }
        const auto& values = ray.fields.at(static_cast<std::size_t>(product_));
        const auto azimuth = qDegreesToRadians(90.0 - static_cast<double>(ray.azimuthDegrees));
        const auto sinAzimuth = std::sin(azimuth);
        const auto cosAzimuth = std::cos(azimuth);

        for (std::size_t gate = 0; gate < values.size(); ++gate) {
            const auto value = nativeValue(product_, values[gate]);
            if (!value.has_value()) {
                continue;
            }
            const auto normalized = std::clamp((*value - descriptor.minimum) /
                                                   (descriptor.maximum - descriptor.minimum),
                                               0.0, 1.0);
            const auto hue = product_ == ProductKind::Reflectivity
                                 ? 0.68 - normalized * 0.68
                                 : 0.58 + normalized * 0.35;
            const auto color = QColor::fromHsvF(std::fmod(hue + 1.0, 1.0), 0.95,
                                                0.25 + normalized * 0.75);
            painter.setPen(QPen(color, 2.0));
            const auto range = static_cast<double>(ray.firstGateMeters) +
                               static_cast<double>(gate) * ray.gateSpacingMeters;
            const auto screenRadius = radius * range / maxRangeMeters;
            painter.drawPoint(QPointF(center.x() + cosAzimuth * screenRadius,
                                      center.y() - sinAzimuth * screenRadius));
        }
    }
}

} // namespace cappedweather
