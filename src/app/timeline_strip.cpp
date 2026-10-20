#include "app/timeline_strip.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>

namespace cappedweather {

TimelineStrip::TimelineStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("timelineStrip"));
    setMinimumHeight(44);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(QStringLiteral("Click a block to jump to that volume · right-click to bookmark it (B)"));
    setMouseTracking(true);
}

void TimelineStrip::setFrames(QVector<Frame> frames, int currentIndex) {
    frames_ = std::move(frames);
    current_ = currentIndex;
    update();
}

void TimelineStrip::setEvents(QVector<QDateTime> eventTimes) {
    events_ = std::move(eventTimes);
    std::sort(events_.begin(), events_.end());
    update();
}

QSize TimelineStrip::sizeHint() const { return {200, 44}; }

QString TimelineStrip::emptyStateText() {
    return QStringLiteral("No volumes yet \u2014 press Play");
}

bool TimelineStrip::showsTimeLabel(int index, int currentIndex, int frameCount) {
    if (frameCount <= 0 || index < 0 || index >= frameCount) return false;
    return index == currentIndex || index == 0 || index + 1 == frameCount;
}

std::optional<double> TimelineStrip::eventTickX(const QDateTime& when, const QDateTime& first,
                                                const QDateTime& last, double width, double gap) {
    if (!when.isValid() || !first.isValid() || !last.isValid()) return std::nullopt;
    if (when < first || when > last) return std::nullopt;
    const auto span = static_cast<double>(first.secsTo(last));
    if (!(span > 0.0) || !(width > 2.0 * gap)) return std::nullopt;
    return gap + (static_cast<double>(first.secsTo(when)) / span) * (width - 2.0 * gap);
}

int TimelineStrip::indexAt(double x) const {
    if (frames_.isEmpty() || width() <= 0) return -1;
    const auto index = static_cast<int>(x / width() * frames_.size());
    return std::clamp(index, 0, static_cast<int>(frames_.size()) - 1);
}

void TimelineStrip::mousePressEvent(QMouseEvent* event) {
    const auto index = indexAt(event->position().x());
    if (index < 0 || index >= frames_.size()) return;
    if (event->button() == Qt::RightButton) emit bookmarkToggled(frames_.at(index).volumeId);
    else if (event->button() == Qt::LeftButton) emit frameSelected(index);
}

void TimelineStrip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(16, 18, 21, 255));
    painter.drawRoundedRect(rect(), 4, 4);
    if (frames_.isEmpty()) {
        painter.setPen(QColor(125, 131, 140));
        painter.drawText(rect(), Qt::AlignCenter, emptyStateText());
        return;
    }
    const double gap = 3.0;
    const double blockWidth =
        (width() - gap * (frames_.size() + 1)) / static_cast<double>(frames_.size());
    const QRectF area(0, 10, width(), height() - 10);
    QDateTime first = frames_.front().time;
    QDateTime last = frames_.back().time;
    const bool timeRange = first.isValid() && last.isValid() && first != last;
    for (int i = 0; i < frames_.size(); ++i) {
        const auto& frame = frames_.at(i);
        const QRectF block(gap + i * (blockWidth + gap), area.top() + 4.0,
                           std::max(2.0, blockWidth), area.height() - 8.0);
        const bool current = i == current_;
        QColor fill = frame.complete ? QColor(74, 126, 166, current ? 230 : 140)
                                     : QColor(150, 80, 70, current ? 230 : 140);
        painter.setBrush(fill);
        painter.setPen(current ? QPen(QColor(230, 240, 245), 2.0) : Qt::NoPen);
        painter.drawRoundedRect(block, 2, 2);
        if (frame.bookmarked) {
            painter.setBrush(QColor(224, 193, 74));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(QRectF(block.left(), block.top() - 4.0, block.width(), 3.0), 1, 1);
        }
        if (frame.time.isValid()) {
            const auto label = frame.time.toLocalTime().toString(QStringLiteral("HH:mm"));
            if (showsTimeLabel(i, current_, frames_.size())) {
                painter.setPen(QColor(200, 208, 214));
                painter.drawText(QRectF(block.left() - 20, area.bottom() - 2, block.width() + 40, 12),
                                 Qt::AlignHCenter | Qt::AlignTop, label);
            }
        }
    }
    // Warning-onset ticks across the loaded time range.
    if (timeRange && !events_.isEmpty()) {
        painter.setPen(QPen(QColor(255, 107, 94), 2.0));
        for (const auto& when : events_) {
            const auto x = eventTickX(when, first, last, width(), gap);
            if (!x.has_value()) continue;
            painter.drawLine(QPointF(*x, 2.0), QPointF(*x, 9.0));
        }
    }
}

} // namespace cappedweather
