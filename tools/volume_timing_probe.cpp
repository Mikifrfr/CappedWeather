// Volume timing probe: cold-start MainWindow (test-mode settings, lean
// opt-out defaults), select one radar, and time the wait for the first
// complete live volume. The user-visible end of the startup path: how long
// until real echo is on screen.
//
// Usage: volume_timing_probe <RADAR>   (default: KGSP)
#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QStandardPaths>
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

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherVolumeProbe"));
    QStandardPaths::setTestModeEnabled(true);

    const QString radar = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("KGSP");

    QElapsedTimer wall;
    wall.start();
    MainWindow window;
    window.resize(1440, 850);
    window.show();
    auto* station = window.findChild<QComboBox*>(QStringLiteral("stationCombo"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("unifiedStatusLabel"));
    auto maps = window.findChildren<RadarMapCanvas*>();
    if (!station || !status || maps.isEmpty()) {
        std::cerr << "timing probe could not find required UI controls\n";
        return 2;
    }
    const int index = station->findData(radar);
    if (index < 0) {
        std::cerr << "radar missing from picker: " << radar.toStdString() << '\n';
        return 3;
    }
    station->setCurrentIndex(index);
    station->activated(index);
    const bool gotVolume = waitUntil(application, 300000, [&] {
        return status->text().contains(QStringLiteral("received 1 complete volume"),
                                       Qt::CaseInsensitive);
    });
    const double wallS = wall.elapsed() / 1000.0;
    std::cout << "PROBE: done volume=" << (gotVolume ? 1 : 0) << " radar=" << radar.toStdString()
              << " wall_s=" << wallS << '\n'
              << "status=" << status->text().toStdString() << '\n';
    return gotVolume ? 0 : 4;
}
