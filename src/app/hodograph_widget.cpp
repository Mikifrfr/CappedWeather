#include "app/hodograph_widget.hpp"

#include <QPainter>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cappedweather {
namespace {

constexpr double kKnotsToMps = 0.5144444444444445;
constexpr double kTopAglMeters = 12000.0;  // color ramp spans the troposphere

QColor heightColor(const double aglMeters) {
    const double fraction = std::clamp(aglMeters / kTopAglMeters, 0.0, 1.0);
    // Blue (low) -> red (high) through purple: readable on dark chrome.
    return QColor::fromHsvF(static_cast<float>(0.66 * (1.0 - fraction)), 0.85f, 0.95f);
}

}  // namespace

HodographWidget::HodographWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(340, 340);
}

void HodographWidget::setSounding(Sounding sounding) {
    sounding_ = std::move(sounding);
    update();
}

void HodographWidget::setStormMotion(const double cxMps, const double cyMps, const bool valid) {
    stormU_ = cxMps;
    stormV_ = cyMps;
    stormValid_ = valid && std::isfinite(cxMps) && std::isfinite(cyMps);
    update();
}

void HodographWidget::paintEvent(QPaintEvent* event) {
    (void)event;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(8, 15, 19));
    const double surfaceMsl = [&] {
        for (const auto& level : sounding_.levels) {
            if (std::isfinite(level.heightMslMeters)) return level.heightMslMeters;
        }
        return std::numeric_limits<double>::quiet_NaN();
    }();
    struct PlotPoint {
        double u = 0.0;
        double v = 0.0;
        double agl = 0.0;
    };
    std::vector<PlotPoint> trace;
    double maxSpeed = 30.0;
    if (std::isfinite(surfaceMsl)) {
        for (const auto& level : sounding_.levels) {
            if (!std::isfinite(level.heightMslMeters) ||
                !std::isfinite(level.directionFromDegrees) ||
                !std::isfinite(level.speedKnots) || level.speedKnots < 0.0)
                continue;
            const double speedMps = level.speedKnots * kKnotsToMps;
            const double toRad =
                (level.directionFromDegrees + 180.0) * 3.14159265358979323846 / 180.0;
            const double u = speedMps * std::sin(toRad);
            const double v = speedMps * std::cos(toRad);
            if (!std::isfinite(u) || !std::isfinite(v)) continue;
            maxSpeed = std::max(maxSpeed, std::hypot(u, v));
            trace.push_back({u, v, level.heightMslMeters - surfaceMsl});
        }
    }
    const QPointF center(width() / 2.0, height() / 2.0);
    const double radius = std::min(width(), height()) / 2.0 - 28.0;
    if (trace.empty()) {
        painter.setPen(QColor(148, 163, 184));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No sounding loaded"));
        return;
    }
    maxSpeed = std::min(120.0, std::max(30.0, std::ceil(maxSpeed / 20.0) * 20.0));
    const auto toScreen = [&](const double u, const double v) {
        return QPointF(center.x() + u / maxSpeed * radius, center.y() - v / maxSpeed * radius);
    };
    // Compass rings at thirds of full scale, labeled in m/s.
    painter.setPen(QPen(QColor(60, 70, 78), 1.0));
    for (int third = 1; third <= 3; ++third) {
        const double ring = radius * third / 3.0;
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, ring, ring);
        painter.setPen(QPen(QColor(120, 130, 140), 1.0));
        painter.drawText(QRectF(center.x() + ring - 44, center.y() - 18, 40, 14),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(maxSpeed * third / 3.0, 'f', 0));
        painter.setPen(QPen(QColor(60, 70, 78), 1.0));
    }
    // Cardinal axes through the origin.
    painter.drawLine(QPointF(center.x() - radius, center.y()), QPointF(center.x() + radius, center.y()));
    painter.drawLine(QPointF(center.x(), center.y() - radius), QPointF(center.x(), center.y() + radius));
    painter.setPen(QColor(120, 130, 140));
    painter.drawText(QRectF(center.x() + radius - 14, center.y() + 4, 20, 14), QStringLiteral("E"));
    painter.drawText(QRectF(center.x() - 8, center.y() - radius - 2, 20, 14), QStringLiteral("N"));
    // Storm-motion vector first (under the trace): dashed orange origin arrow.
    if (stormValid_) {
        const auto tip = toScreen(stormU_, stormV_);
        painter.setPen(QPen(QColor(255, 170, 60), 1.5, Qt::DashLine));
        painter.drawLine(center, tip);
        painter.setPen(QPen(QColor(255, 170, 60), 2.0));
        painter.drawLine(tip + QPointF(-4, -4), tip + QPointF(4, 4));
        painter.drawLine(tip + QPointF(-4, 4), tip + QPointF(4, -4));
    }
    // Wind trace: height-colored segments, fat dots on the 0-3 km limb.
    for (std::size_t i = 1; i < trace.size(); ++i) {
        painter.setPen(QPen(heightColor(trace[i].agl), 2.0));
        painter.drawLine(toScreen(trace[i - 1].u, trace[i - 1].v),
                         toScreen(trace[i].u, trace[i].v));
    }
    for (const auto& point : trace) {
        const bool lowLevel = point.agl <= 3000.0;
        painter.setPen(QPen(QColor(10, 14, 17), 1.0));
        painter.setBrush(heightColor(point.agl));
        const auto dot = toScreen(point.u, point.v);
        painter.drawEllipse(dot, lowLevel ? 3.5 : 2.0, lowLevel ? 3.5 : 2.0);
    }
}

}  // namespace cappedweather
