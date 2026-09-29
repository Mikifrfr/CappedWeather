#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>
#include <QWidget>

namespace cappedweather {

// Filmstrip timeline: one block per loaded volume under the transport
// buttons. Click jumps, right-click bookmarks. Warning onsets inside the
// loaded range draw as red ticks so a chase day shows *why* a frame
// matters, not just *when* it is.
class TimelineStrip : public QWidget {
    Q_OBJECT

public:
    struct Frame {
        QDateTime time;
        bool complete = true;
        bool bookmarked = false;
        QString volumeId;
    };

    explicit TimelineStrip(QWidget* parent = nullptr);

    void setFrames(QVector<Frame> frames, int currentIndex);
    void setEvents(QVector<QDateTime> eventTimes);

signals:
    void frameSelected(int index);
    void bookmarkToggled(const QString& volumeId);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    int indexAt(double x) const;

    QVector<Frame> frames_;
    QVector<QDateTime> events_;
    int current_ = -1;
};

} // namespace cappedweather
