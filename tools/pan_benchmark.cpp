// Pan benchmark: scripted drag-pans on live data to measure echo repaint cost.
//
// Opens the real MainWindow against live public NOAA endpoints, selects a
// radar, waits for a complete volume, lets the overlay build settle, then
// performs synthetic middle-button drag-pans on panel 0. The temporary
// [echo-profile]/[paint-profile] instrumentation in radar_map_canvas logs
// rebuild + paint costs to stdout; this tool logs wall time per drag.
//
// Usage: pan_benchmark [radar] [drags]   (default: KJAX 3)
// No synthetic data is ever injected: without a live volume the tool fails.
#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QStandardPaths>
#include <QTest>
#include <QThread>

#include <iostream>

using namespace cappedweather;

namespace {

bool waitUntil(QApplication& application, const int timeoutMs, const auto& predicate) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMs) {
        application.processEvents(QEventLoop::AllEvents, 100);
        if (predicate()) return true;
        QThread::msleep(100);
    }
    application.processEvents(QEventLoop::AllEvents, 100);
    return predicate();
}

void settle(QApplication& application, int ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < ms) {
        application.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(50);
    }
}

bool selectRadar(QComboBox* station, const QString& radarId) {
    const int index = station->findData(radarId);
    if (index < 0) {
        std::cerr << "radar missing from picker: " << radarId.toStdString() << '\n';
        return false;
    }
    station->setCurrentIndex(index);
    station->activated(index);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherPanBench"));
    QStandardPaths::setTestModeEnabled(true);

    const QString radar = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("KJAX");
    const int drags = argc > 2 ? std::max(1, QString::fromLocal8Bit(argv[2]).toInt()) : 3;

    MainWindow window;
    window.resize(1100, 750);
    window.show();
    window.move(10000, 500);

    auto* station = window.findChild<QComboBox*>(QStringLiteral("stationCombo"));
    auto* layout = window.findChild<QComboBox*>(QStringLiteral("panelLayoutCombo"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("unifiedStatusLabel"));
    auto maps = window.findChildren<RadarMapCanvas*>();
    std::sort(maps.begin(), maps.end(), [](const RadarMapCanvas* a, const RadarMapCanvas* b) {
        return a->objectName() < b->objectName();
    });
    if (!station || !layout || !status || maps.isEmpty()) {
        std::cerr << "benchmark could not find required UI controls\n";
        return 2;
    }
    // Single panel: only panel 0 paints, so the profile lines belong to it.
    layout->setCurrentIndex(0);
    if (!selectRadar(station, radar)) return 3;
    if (!waitUntil(application, 360000, [&] {
            return maps[0]->radarId() == radar &&
                   status->text().contains(QStringLiteral("complete volume"),
                                           Qt::CaseInsensitive);
        })) {
        std::cerr << "volume timeout for " << radar.toStdString() << ": "
                  << status->text().toStdString() << '\n';
        return 4;
    }
    std::cout << "BENCH: volume ready, settling for overlay build\n";
    settle(application, 10000);
    std::cout << "BENCH: starting " << drags << " drags\n";
    if (!maps[0]->grab().save(QStringLiteral("C:/Users/brawl/AppData/Local/Temp/opencode/panbench_echo_before.png"),
                              "PNG")) {
        std::cerr << "could not save pre-drag echo grab\n";
        return 5;
    }
    std::cout << "BENCH: pre-drag echo grab saved\n";

    const QPoint start = maps[0]->rect().center();
    for (int drag = 0; drag < drags; ++drag) {
        QElapsedTimer dragClock;
        dragClock.start();
        // Middle-button drag: pans without risking a click on a polygon.
        QTest::mousePress(maps[0], Qt::MiddleButton, Qt::NoModifier, start);
        for (int step = 1; step <= 20; ++step) {
            QTest::mouseMove(maps[0], start + QPoint(step * 15, step * 8));
            QTest::qWait(16);
        }
        QTest::mouseRelease(maps[0], Qt::MiddleButton, Qt::NoModifier,
                            start + QPoint(300, 160));
        settle(application, 2500); // let stale rebuilds drain before the next drag
        std::cout << "BENCH: drag " << drag << " wall=" << dragClock.elapsed() << "ms\n";
    }
    std::cout << "BENCH: done\n";
    if (!maps[0]->grab().save(QStringLiteral("C:/Users/brawl/AppData/Local/Temp/opencode/panbench_echo.png"),
                              "PNG")) {
        std::cerr << "could not save echo grab\n";
        return 5;
    }
    std::cout << "BENCH: echo grab saved\n";
    return 0;
}
