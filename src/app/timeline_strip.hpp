#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>
#include <QWidget>

#include <optional>

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

    // Spec hooks (pure, hermetic-testable): the empty state carries no LIVE
    // wording, HH:mm labels draw on first/current/last only, and warning
    // onsets inside [first, last] map to tick x positions.
    static QString emptyStateText();
    static bool showsTimeLabel(int index, int currentIndex, int frameCount);
    static std::optional<double> eventTickX(const QDateTime& when, const QDateTime& first,
                                            const QDateTime& last, double width,
                                            double gap = 3.0);

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
