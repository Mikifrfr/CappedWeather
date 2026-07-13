#include "app/radar_map_canvas.hpp"

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QTimer>
#include <QToolTip>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>

namespace cappedweather {

namespace {
constexpr double kPi = 3.14159265358979323846;

struct ColorStop {
    double position;
    QColor color;
};

QColor interpolatePalette(const std::initializer_list<ColorStop> stops, const double position) {
    const auto value = std::clamp(position, 0.0, 1.0);
    auto previous = stops.begin();
    for (auto current = std::next(previous); current != stops.end(); ++current) {
        if (value > current->position) {
            previous = current;
            continue;
        }
        const auto span = current->position - previous->position;
        const auto amount = span > 0.0 ? (value - previous->position) / span : 0.0;
        const auto channel = [amount](const int from, const int to) {
            return static_cast<int>(std::round(from + (to - from) * amount));
        };
        return QColor(channel(previous->color.red(), current->color.red()),
                      channel(previous->color.green(), current->color.green()),
                      channel(previous->color.blue(), current->color.blue()), 205);
    }
    auto result = std::prev(stops.end())->color;
    result.setAlpha(205);
    return result;
}

QColor productColor(const ProductKind product, const double normalized) {
    switch (product) {
    case ProductKind::Reflectivity:
        return interpolatePalette({
            {0.00, QColor("#102b3a")}, {0.16, QColor("#146b78")},
            {0.30, QColor("#35c7b4")}, {0.44, QColor("#b7e75c")},
            {0.58, QColor("#ffd166")}, {0.72, QColor("#ff704d")},
            {0.86, QColor("#d946a8")}, {1.00, QColor("#f2e9ff")},
        }, normalized);
    case ProductKind::Velocity:
        return interpolatePalette({
            {0.00, QColor("#38156b")}, {0.18, QColor("#304fc4")},
            {0.36, QColor("#16a6d8")}, {0.48, QColor("#63eee0")},
            {0.495, QColor("#24343b")}, {0.505, QColor("#24343b")},
            {0.52, QColor("#ffe07a")}, {0.66, QColor("#ff9b42")},
            {0.84, QColor("#ef3f55")}, {1.00, QColor("#9e185f")},
        }, normalized);
    case ProductKind::SpectrumWidth:
        return interpolatePalette({
            {0.00, QColor("#17252d")}, {0.16, QColor("#14637c")},
            {0.32, QColor("#16b6c5")}, {0.48, QColor("#66d36e")},
            {0.64, QColor("#e7d44f")}, {0.78, QColor("#f58a3c")},
            {0.90, QColor("#d9365e")}, {1.00, QColor("#f4eaff")},
        }, normalized);
    case ProductKind::DifferentialReflectivity:
        return interpolatePalette({
            {0.00, QColor("#392568")}, {0.22, QColor("#3978b8")},
            {0.42, QColor("#54c8bd")}, {0.50, QColor("#dbe4df")},
            {0.66, QColor("#efb45d")}, {0.84, QColor("#e45a78")},
            {1.00, QColor("#9f3a96")},
        }, normalized);
    case ProductKind::CorrelationCoefficient:
        return interpolatePalette({
            {0.00, QColor("#76264c")}, {0.35, QColor("#d5525c")},
            {0.62, QColor("#e8a957")}, {0.78, QColor("#7779c9")},
            {0.90, QColor("#2eb7bd")}, {0.97, QColor("#a8e4d7")},
            {1.00, QColor("#edf7f2")},
        }, normalized);
    case ProductKind::DifferentialPhase:
        return interpolatePalette({
            {0.00, QColor("#234c89")}, {0.18, QColor("#2cb5b4")},
            {0.36, QColor("#9cd65f")}, {0.52, QColor("#f2cf58")},
            {0.68, QColor("#ee7454")}, {0.84, QColor("#bd4ca5")},
            {1.00, QColor("#51418d")},
        }, normalized);
    }
    return QColor("#d7e2e6");
}

int wrappedTileX(const int x, const int count) {
    return ((x % count) + count) % count;
}
}

RadarMapCanvas::RadarMapCanvas(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("radarMap"));
    setMinimumSize(520, 420);
    setMouseTracking(true);
    connect(&network_, &QNetworkAccessManager::finished, this, &RadarMapCanvas::tileFinished);
    auto* loadingTimer = new QTimer(this);
    loadingTimer->setInterval(50);
    connect(loadingTimer, &QTimer::timeout, this, [this] {
        if (!loading_) return;
        loadingAngle_ = (loadingAngle_ + 18) % 360;
        update();
    });
    loadingTimer->start();
}

void RadarMapCanvas::setSites(std::vector<RadarSite> sites) {
    sites_ = std::move(sites);
    rebuildOverlay();
    update();
}

void RadarMapCanvas::setSelectedRadar(const QString& radarId) {
    const auto hadSelection = !selectedRadar_.isEmpty();
    const auto changed = selectedRadar_ != radarId;
    selectedRadar_ = radarId;
    if (changed) {
        chunk_ = {};
        overlayPaths_.clear();
        overlayZoom_ = -1;
        dataTime_ = {};
        probeText_.clear();
        if (hadSelection) {
            const auto* site = selectedSite();
            if (!site || !site->latitude || !site->longitude) {
                update();
                return;
            }
            center_ = {site->longitude.value(), site->latitude.value()};
            zoom_ = 6;
        }
    }
    update();
}

void RadarMapCanvas::setProduct(const ProductKind product) {
    product_ = product;
    rebuildOverlay();
    update();
}

void RadarMapCanvas::setElevationFilter(const std::optional<int> elevationNumber) {
    elevationFilter_ = elevationNumber;
    rebuildOverlay();
    update();
}

void RadarMapCanvas::setDecodedChunk(DecodedChunk chunk, const QDateTime& dataTime) {
    chunk_ = std::move(chunk);
    dataTime_ = dataTime;
    rebuildOverlay();
    setLoading(false);
    update();
}

void RadarMapCanvas::setWeatherOverlays(QVector<WeatherWarning> warnings,
                                        QVector<StormReport> reports) {
    warnings_ = std::move(warnings);
    stormReports_ = std::move(reports);
    update();
}

void RadarMapCanvas::setWarningsVisible(const bool visible) {
    warningsVisible_ = visible;
    update();
}

void RadarMapCanvas::setStormReportsVisible(const bool visible) {
    stormReportsVisible_ = visible;
    update();
}

void RadarMapCanvas::setLoading(const bool loading, const QString& message) {
    loading_ = loading;
    loadingMessage_ = message;
    if (loading) loadingAngle_ = 0;
    update();
}

QString RadarMapCanvas::radarId() const {
    return selectedRadar_;
}

ProductKind RadarMapCanvas::product() const {
    return product_;
}

QString RadarMapCanvas::probeText() const {
    return probeText_;
}

int RadarMapCanvas::warningCount() const {
    return warnings_.size();
}

int RadarMapCanvas::stormReportCount() const {
    return stormReports_.size();
}

QPointF RadarMapCanvas::worldPoint(const double latitude, const double longitude) const {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto lat = qDegreesToRadians(std::clamp(latitude, -85.0511, 85.0511));
    const auto sinLat = std::sin(lat);
    const auto x = (longitude + 180.0) / 360.0 * world;
    const auto y = (0.5 - std::log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * kPi)) * world;
    return {x, y};
}

QPointF RadarMapCanvas::screenPoint(const double latitude, const double longitude) const {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = worldPoint(center_.y(), center_.x());
    auto point = worldPoint(latitude, longitude);
    auto dx = point.x() - center.x();
    if (dx > world / 2.0) dx -= world;
    if (dx < -world / 2.0) dx += world;
    point.setX(width() / 2.0 + dx);
    point.setY(height() / 2.0 + point.y() - center.y());
    return point;
}

QPointF RadarMapCanvas::geographicPoint(const QPointF& screen) const {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = worldPoint(center_.y(), center_.x());
    auto x = center.x() + screen.x() - width() / 2.0;
    x = std::fmod(x + world, world);
    if (x < 0.0) x += world;
    const auto y = std::clamp(center.y() + screen.y() - height() / 2.0, 0.0, world);
    const auto longitude = x / world * 360.0 - 180.0;
    const auto n = kPi - 2.0 * kPi * y / world;
    return {longitude, qRadiansToDegrees(std::atan(std::sinh(n)))};
}

QPoint RadarMapCanvas::radarScreenPoint(const QString& radarId) const {
    for (const auto& site : sites_) {
        if (QString::fromStdString(site.id) == radarId && site.latitude && site.longitude) {
            return screenPoint(*site.latitude, *site.longitude).toPoint();
        }
    }
    return {};
}

const RadarSite* RadarMapCanvas::selectedSite() const {
    for (const auto& site : sites_) {
        if (QString::fromStdString(site.id) == selectedRadar_) {
            return &site;
        }
    }
    return nullptr;
}

void RadarMapCanvas::rebuildOverlay() {
    overlayPaths_.clear();
    overlayZoom_ = zoom_;
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude) {
        return;
    }

    const auto& descriptor = productDescriptor(product_);
    const auto baseLatitude = *site->latitude;
    const auto baseLongitude = *site->longitude;
    const auto cosLatitude = std::cos(qDegreesToRadians(baseLatitude));
    if (std::abs(cosLatitude) < 0.01) {
        return;
    }

    for (const auto& ray : chunk_.rays) {
        if (elevationFilter_.has_value() && ray.elevationNumber != *elevationFilter_) {
            continue;
        }
        if (!ray.has(product_)) {
            continue;
        }

        const auto& values = ray.fields.at(static_cast<std::size_t>(product_));
        const auto productIndex = static_cast<std::size_t>(product_);
        const auto firstGateMeters = ray.firstGateByProduct.at(productIndex);
        const auto gateSpacingMeters = ray.gateSpacingByProduct.at(productIndex);
        if (gateSpacingMeters == 0) {
            continue;
        }
        const auto halfAzimuth = std::clamp(
            static_cast<double>(ray.azimuthResolutionDegrees) * 0.5, 0.1, 0.75);
        const auto halfRange = static_cast<double>(gateSpacingMeters) * 0.5;
        for (std::size_t gate = 0; gate < values.size(); ++gate) {
            const auto value = nativeValue(values[gate],
                                           ray.scaleByProduct.at(productIndex),
                                           ray.offsetByProduct.at(productIndex));
            if (!value.has_value()) {
                continue;
            }
            const auto normalized = std::clamp((*value - descriptor.minimum) /
                                                   (descriptor.maximum - descriptor.minimum),
                                               0.0, 1.0);
            const auto color = productColor(product_, std::round(normalized * 63.0) / 63.0);
            const auto rangeCenter = static_cast<double>(firstGateMeters) +
                                     (static_cast<double>(gate) + 0.5) * gateSpacingMeters;
            const auto lowerRange = std::max(0.0, rangeCenter - halfRange);
            const auto upperRange = rangeCenter + halfRange;
            if (upperRange <= 0.0) {
                continue;
            }
            const auto coordinateAt = [&](const double range, const double azimuth) {
                const auto bearing = qDegreesToRadians(azimuth);
                const auto northDegrees = std::cos(bearing) * range / 111320.0;
                const auto eastDegrees = std::sin(bearing) * range / (111320.0 * cosLatitude);
                return QPointF(baseLongitude + eastDegrees, baseLatitude + northDegrees);
            };
            const auto lowerAzimuth = static_cast<double>(ray.azimuthDegrees) - halfAzimuth;
            const auto upperAzimuth = static_cast<double>(ray.azimuthDegrees) + halfAzimuth;
            const std::array coordinates{
                coordinateAt(lowerRange, lowerAzimuth),
                coordinateAt(upperRange, lowerAzimuth),
                coordinateAt(upperRange, upperAzimuth),
                coordinateAt(lowerRange, upperAzimuth),
            };
            QPolygonF polygon;
            polygon.reserve(4);
            for (const auto& coordinate : coordinates) {
                polygon.push_back(worldPoint(coordinate.y(), coordinate.x()));
            }
            overlayPaths_[color.rgba()].addPolygon(polygon);
        }
    }
}

void RadarMapCanvas::paintRadarOverlay(QPainter& painter) {
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude) {
        return;
    }

    if (overlayZoom_ != zoom_) rebuildOverlay();
    painter.save();
    if (!overlayPaths_.empty()) {
        const auto mapCenter = worldPoint(center_.y(), center_.x());
        painter.translate(width() / 2.0 - mapCenter.x(), height() / 2.0 - mapCenter.y());
        painter.setPen(Qt::NoPen);
        for (auto it = overlayPaths_.cbegin(); it != overlayPaths_.cend(); ++it) {
            painter.setBrush(QColor::fromRgba(it.key()));
            painter.drawPath(it.value());
        }
    }
    painter.restore();

    painter.save();
    painter.setPen(QPen(QColor(255, 255, 255, 150), 1));
    painter.setBrush(Qt::NoBrush);
    const auto center = screenPoint(*site->latitude, *site->longitude);
    const auto edge = screenPoint(*site->latitude, *site->longitude +
                                  230000.0 / (111320.0 * std::cos(qDegreesToRadians(*site->latitude))));
    const auto radius = std::abs(edge.x() - center.x());
    painter.drawEllipse(center, radius, radius);

    painter.restore();
}

void RadarMapCanvas::paintWeatherOverlays(QPainter& painter) {
    if (warningsVisible_) {
        for (const auto& warning : warnings_) {
            QColor color("#f4d35e");
            if (warning.event.contains(QStringLiteral("Tornado"), Qt::CaseInsensitive)) color = QColor("#ff3b4d");
            else if (warning.event.contains(QStringLiteral("Severe Thunderstorm"), Qt::CaseInsensitive)) color = QColor("#ff9f43");
            else if (warning.event.contains(QStringLiteral("Flash Flood"), Qt::CaseInsensitive)) color = QColor("#36d17c");
            painter.setPen(QPen(color, 2.0));
            auto fill = color;
            fill.setAlpha(35);
            painter.setBrush(fill);
            for (const auto& geographicPolygon : warning.polygons) {
                QPolygonF polygon;
                polygon.reserve(geographicPolygon.size());
                for (const auto& coordinate : geographicPolygon) {
                    polygon.push_back(screenPoint(coordinate.y(), coordinate.x()));
                }
                painter.drawPolygon(polygon);
            }
        }
    }

    if (stormReportsVisible_) {
        painter.setPen(QPen(QColor(245, 248, 249), 1.0));
        for (const auto& report : stormReports_) {
            const auto point = screenPoint(report.coordinate.y(), report.coordinate.x());
            if (!rect().adjusted(-10, -10, 10, 10).contains(point.toPoint())) continue;
            if (report.kind == StormReportKind::Tornado) {
                painter.setBrush(QColor("#ff3b4d"));
                painter.drawPolygon(QPolygonF{{point.x(), point.y() - 6.0},
                                               {point.x() - 5.5, point.y() + 5.0},
                                               {point.x() + 5.5, point.y() + 5.0}});
            } else if (report.kind == StormReportKind::Hail) {
                painter.setBrush(QColor("#53d769"));
                painter.drawEllipse(point, 5.0, 5.0);
            } else {
                painter.setBrush(QColor("#44a7ff"));
                painter.drawRect(QRectF(point.x() - 4.5, point.y() - 4.5, 9.0, 9.0));
            }
        }
    }
}

void RadarMapCanvas::paintLegend(QPainter& painter) {
    const auto& descriptor = productDescriptor(product_);
    const QRectF panel(12, 12, 258, 50);
    painter.setPen(QPen(QColor(205, 216, 222, 130), 1));
    painter.setBrush(QColor(8, 15, 19, 220));
    painter.drawRoundedRect(panel, 4, 4);
    painter.setPen(QColor(235, 240, 243));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold));
    painter.drawText(QRectF(20, 17, 242, 14), Qt::AlignLeft,
                     QStringLiteral("%1 (%2)")
                         .arg(QString::fromUtf8(descriptor.label.data()),
                              QString::fromUtf8(descriptor.unit.data())));
    const QRectF gradientRect(20, 34, 242, 10);
    QLinearGradient gradient(gradientRect.topLeft(), gradientRect.topRight());
    for (int step = 0; step <= 16; ++step) {
        const auto position = step / 16.0;
        gradient.setColorAt(position, productColor(product_, position));
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRect(gradientRect);
    painter.setPen(QColor(205, 215, 220));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 7));
    painter.drawText(QRectF(20, 45, 80, 12), Qt::AlignLeft, QString::number(descriptor.minimum, 'g', 4));
    painter.drawText(QRectF(182, 45, 80, 12), Qt::AlignRight, QString::number(descriptor.maximum, 'g', 4));
}

void RadarMapCanvas::paintProbe(QPainter& painter) {
    if (probeText_.isEmpty()) return;
    const auto metrics = painter.fontMetrics();
    const auto textWidth = metrics.horizontalAdvance(probeText_);
    auto x = probePosition_.x() + 14.0;
    auto y = probePosition_.y() + 14.0;
    if (x + textWidth + 18.0 > width()) x = probePosition_.x() - textWidth - 18.0;
    if (y + 28.0 > height()) y = probePosition_.y() - 30.0;
    const QRectF box(x, y, textWidth + 12.0, 23.0);
    painter.setPen(QPen(QColor(210, 220, 225), 1));
    painter.setBrush(QColor(5, 11, 14, 235));
    painter.drawRoundedRect(box, 3, 3);
    painter.setPen(QColor(242, 246, 248));
    painter.drawText(box.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, probeText_);
}

QString RadarMapCanvas::tileKey(const int x, const int y) const {
    return QStringLiteral("%1/%2/%3").arg(zoom_).arg(x).arg(y);
}

void RadarMapCanvas::requestVisibleTiles() {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = worldPoint(center_.y(), center_.x());
    const auto left = center.x() - width() / 2.0;
    const auto top = center.y() - height() / 2.0;
    const auto firstX = static_cast<int>(std::floor(left / kTileSize));
    const auto lastX = static_cast<int>(std::floor((left + width()) / kTileSize));
    const auto firstY = std::max(0, static_cast<int>(std::floor(top / kTileSize)));
    const auto lastY = std::min((1 << zoom_) - 1,
                                static_cast<int>(std::floor((top + height()) / kTileSize)));
    const auto tileCount = 1 << zoom_;

    for (int tileY = firstY; tileY <= lastY; ++tileY) {
        for (int tileX = firstX; tileX <= lastX; ++tileX) {
            const auto wrappedX = wrappedTileX(tileX, tileCount);
            const auto key = tileKey(wrappedX, tileY);
            if (tiles_.contains(key) || pendingTiles_.values().contains(key)) {
                continue;
            }
            const auto url = QUrl(QStringLiteral("https://tile.openstreetmap.org/%1/%2/%3.png")
                                      .arg(zoom_).arg(wrappedX).arg(tileY));
            QNetworkRequest request(url);
            request.setHeader(QNetworkRequest::UserAgentHeader,
                              QStringLiteral("CappedWeather/0.1 (NEXRAD viewer; contact@example.invalid)"));
            pendingTiles_.insert(network_.get(request), key);
        }
    }
    Q_UNUSED(world);
}

void RadarMapCanvas::tileFinished(QNetworkReply* reply) {
    const auto key = pendingTiles_.take(reply);
    if (reply->error() != QNetworkReply::NoError) {
    } else {
        const auto image = QImage::fromData(reply->readAll(), "PNG");
        if (!image.isNull()) {
            tiles_.insert(key, image);
        }
    }
    reply->deleteLater();
    update();
}

void RadarMapCanvas::panBy(const QPointF& delta) {
    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = worldPoint(center_.y(), center_.x()) - delta;
    const auto wrappedX = std::fmod(center.x() + world, world);
    const auto x = wrappedX < 0.0 ? wrappedX + world : wrappedX;
    const auto normalizedY = std::clamp(center.y(), 0.0, world);
    const auto longitude = x / world * 360.0 - 180.0;
    const auto n = kPi - 2.0 * kPi * normalizedY / world;
    const auto latitude = qRadiansToDegrees(std::atan(std::sinh(n)));
    center_ = {longitude, latitude};
    update();
}

QString RadarMapCanvas::radarAt(const QPointF& point) const {
    for (const auto& site : sites_) {
        if (!site.latitude || !site.longitude) continue;
        const auto marker = screenPoint(*site.latitude, *site.longitude);
        const QRectF pill(marker.x() - 24.0, marker.y() - 11.0, 48.0, 22.0);
        if (pill.contains(point)) return QString::fromStdString(site.id);
    }
    return {};
}

void RadarMapCanvas::updateProbe(const QPointF& point) {
    probePosition_ = point;
    probeText_.clear();
    const auto* site = selectedSite();
    if (!site || !site->latitude || !site->longitude || chunk_.rays.empty()) {
        update();
        return;
    }

    const auto geographic = geographicPoint(point);
    const auto northMeters = (geographic.y() - *site->latitude) * 111320.0;
    const auto eastMeters = (geographic.x() - *site->longitude) * 111320.0 *
                            std::cos(qDegreesToRadians(*site->latitude));
    const auto rangeMeters = std::hypot(northMeters, eastMeters);
    auto azimuth = qRadiansToDegrees(std::atan2(eastMeters, northMeters));
    if (azimuth < 0.0) azimuth += 360.0;

    const DecodedRay* nearestRay = nullptr;
    auto nearestDifference = 360.0;
    for (const auto& ray : chunk_.rays) {
        if (elevationFilter_.has_value() && ray.elevationNumber != *elevationFilter_) continue;
        if (!ray.has(product_)) continue;
        auto difference = std::abs(static_cast<double>(ray.azimuthDegrees) - azimuth);
        difference = std::min(difference, 360.0 - difference);
        if (difference < nearestDifference) {
            nearestDifference = difference;
            nearestRay = &ray;
        }
    }
    if (!nearestRay || nearestDifference > std::max(0.8, nearestRay->azimuthResolutionDegrees * 0.75)) {
        update();
        return;
    }

    const auto index = static_cast<std::size_t>(product_);
    const auto spacing = nearestRay->gateSpacingByProduct.at(index);
    if (spacing == 0) {
        update();
        return;
    }
    const auto firstGate = nearestRay->firstGateByProduct.at(index);
    const auto gatePosition = (rangeMeters - firstGate) / spacing - 0.5;
    const auto gate = static_cast<long long>(std::llround(gatePosition));
    const auto& values = nearestRay->fields.at(index);
    if (gate < 0 || static_cast<std::size_t>(gate) >= values.size()) {
        update();
        return;
    }

    const auto encoded = values.at(static_cast<std::size_t>(gate));
    QString valueText;
    if (encoded == 0) {
        valueText = QStringLiteral("Below threshold");
    } else if (encoded == 1) {
        valueText = QStringLiteral("Range folded");
    } else if (const auto value = nativeValue(encoded, nearestRay->scaleByProduct.at(index),
                                               nearestRay->offsetByProduct.at(index)); value.has_value()) {
        const auto unit = QString::fromUtf8(productDescriptor(product_).unit.data());
        valueText = QStringLiteral("%1%2%3")
                        .arg(*value, 0, 'f', product_ == ProductKind::CorrelationCoefficient ? 3 : 1)
                        .arg(unit.isEmpty() ? QString{} : QStringLiteral(" "))
                        .arg(unit);
    } else {
        valueText = QStringLiteral("No data");
    }
    probeText_ = QStringLiteral("%1 | %2 km | %3%4 | tilt %5%4")
                     .arg(valueText)
                     .arg(rangeMeters / 1000.0, 0, 'f', 1)
                     .arg(azimuth, 0, 'f', 1)
                     .arg(QChar(0x00b0))
                     .arg(nearestRay->elevationDegrees, 0, 'f', 1);
    update();
}

void RadarMapCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(18, 24, 28));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const auto world = static_cast<double>(kTileSize * (1 << zoom_));
    const auto center = worldPoint(center_.y(), center_.x());
    const auto left = center.x() - width() / 2.0;
    const auto top = center.y() - height() / 2.0;
    const auto firstX = static_cast<int>(std::floor(left / kTileSize));
    const auto lastX = static_cast<int>(std::floor((left + width()) / kTileSize));
    const auto firstY = std::max(0, static_cast<int>(std::floor(top / kTileSize)));
    const auto lastY = std::min((1 << zoom_) - 1,
                                static_cast<int>(std::floor((top + height()) / kTileSize)));
    const auto tileCount = 1 << zoom_;
    for (int tileY = firstY; tileY <= lastY; ++tileY) {
        for (int tileX = firstX; tileX <= lastX; ++tileX) {
            const auto key = tileKey(wrappedTileX(tileX, tileCount), tileY);
            const QRectF target(tileX * kTileSize - left, tileY * kTileSize - top, kTileSize, kTileSize);
            if (tiles_.contains(key)) {
                painter.drawImage(target, tiles_.value(key));
            } else {
                painter.fillRect(target, QColor(28, 36, 41));
                painter.setPen(QColor(47, 59, 66));
                painter.drawRect(target);
            }
        }
    }
    requestVisibleTiles();
    paintRadarOverlay(painter);
    paintWeatherOverlays(painter);

    painter.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold));
    for (const auto& site : sites_) {
        if (!site.latitude || !site.longitude) continue;
        const auto point = screenPoint(*site.latitude, *site.longitude);
        if (!rect().adjusted(-50, -30, 50, 30).contains(point.toPoint())) continue;
        const auto id = QString::fromStdString(site.id);
        const QRectF pill(point.x() - 24.0, point.y() - 11.0, 48.0, 22.0);
        painter.setPen(QPen(id == selectedRadar_ ? QColor(128, 211, 255) : QColor(210, 220, 224),
                            id == selectedRadar_ ? 2.0 : 1.0));
        painter.setBrush(id == selectedRadar_ ? QColor(16, 74, 101, 235) : QColor(15, 27, 34, 225));
        painter.drawRoundedRect(pill, 5, 5);
        painter.setPen(id == selectedRadar_ ? QColor(235, 248, 255) : QColor(230, 235, 238));
        painter.drawText(pill, Qt::AlignCenter, id);
    }

    if (!chunk_.rays.empty()) {
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 9));
        painter.setPen(QColor(232, 238, 240));
        painter.setBrush(QColor(8, 15, 19, 205));
        painter.drawRoundedRect(QRectF(12, height() - 30, 510, 22), 4, 4);
        const auto dataDate = dataTime_.isValid()
                                  ? dataTime_.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss t"))
                                  : QStringLiteral("time unavailable");
        const auto overlayLabel = QStringLiteral("%1 real rays  |  NOAA data: %2")
                                      .arg(chunk_.rays.size()).arg(dataDate);
        painter.drawText(QRectF(20, height() - 27, 494, 16), Qt::AlignLeft, overlayLabel);
    }
    painter.setPen(QColor(35, 45, 50));
    painter.setBrush(QColor(245, 248, 249, 225));
    painter.drawRoundedRect(QRectF(width() - 250, height() - 25, 238, 18), 3, 3);
    painter.setPen(QColor(30, 38, 42));
    painter.drawText(QRectF(width() - 242, height() - 22, 222, 13), Qt::AlignRight,
                     QStringLiteral("(c) OpenStreetMap contributors"));
    paintLegend(painter);
    paintProbe(painter);

    if (loading_) {
        painter.fillRect(rect(), QColor(4, 9, 12, 145));
        const QPointF center(width() / 2.0, height() / 2.0 - 12.0);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(128, 211, 255), 5, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(center.x() - 22, center.y() - 22, 44, 44),
                        loadingAngle_ * 16, 250 * 16);
        painter.setPen(QColor(235, 243, 247));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
        painter.drawText(QRectF(0, center.y() + 34, width(), 22), Qt::AlignCenter,
                         loadingMessage_.isEmpty() ? QStringLiteral("Loading radar data")
                                                   : loadingMessage_);
    }
}

void RadarMapCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        dragged_ = false;
        lastMousePosition_ = event->pos();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void RadarMapCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (dragging_) {
        const auto delta = event->pos() - lastMousePosition_;
        if (!delta.isNull()) {
            dragged_ = true;
            panBy(delta);
            lastMousePosition_ = event->pos();
        }
        event->accept();
        return;
    }
    updateProbe(event->position());
    event->accept();
    return;
}

void RadarMapCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        if (!dragged_) {
            const auto radarId = radarAt(event->pos());
            if (!radarId.isEmpty()) emit radarClicked(radarId);
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void RadarMapCanvas::wheelEvent(QWheelEvent* event) {
    const auto steps = event->angleDelta().y() / 120;
    if (steps == 0) return;
    const auto oldWorld = static_cast<double>(kTileSize * (1 << zoom_));
    const auto cursor = event->position();
    const auto oldCenter = worldPoint(center_.y(), center_.x());
    const auto cursorWorld = oldCenter + cursor - QPointF(width() / 2.0, height() / 2.0);
    zoom_ = std::clamp(zoom_ + steps, kMinZoom, kMaxZoom);
    const auto newWorld = static_cast<double>(kTileSize * (1 << zoom_));
    const auto factor = newWorld / oldWorld;
    const auto newCenter = cursorWorld * factor - (cursor - QPointF(width() / 2.0, height() / 2.0));
    const auto longitude = newCenter.x() / newWorld * 360.0 - 180.0;
    const auto n = kPi - 2.0 * kPi * newCenter.y() / newWorld;
    center_ = {longitude, qRadiansToDegrees(std::atan(std::sinh(n)))};
    rebuildOverlay();
    updateProbe(cursor);
    update();
    event->accept();
}

void RadarMapCanvas::leaveEvent(QEvent* event) {
    probeText_.clear();
    update();
    QWidget::leaveEvent(event);
}

} // namespace cappedweather
