// Overlay cold/warm probe: runs one WeatherOverlaySource refresh against
// live endpoints and prints every statusChanged tally plus wall time.
// Run twice: the first launch downloads zone geometry, the second reuses
// the disk cache and should issue far fewer requests. Test-mode settings
// keep the real user's cache untouched; the probe's own cache dir persists
// across runs by org/app name.
//
// Usage: overlay_probe   (exit 0 when a tally line arrives)
#include "app/weather_overlay_source.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QThread>

#include <iostream>

using namespace cappedweather;

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherOverlayProbe"));
    QStandardPaths::setTestModeEnabled(true);

    WeatherOverlaySource source;
    QString lastStatus;
    QElapsedTimer sinceChange;
    sinceChange.start();
    QObject::connect(&source, &WeatherOverlaySource::statusChanged, [&](const QString& status) {
        lastStatus = status;
        sinceChange.restart();
        std::cout << "PROBE: t=" << sinceChange.elapsed()
                  << "ms status=" << status.toStdString() << '\n';
    });
    QObject::connect(&source, &WeatherOverlaySource::overlaysReady,
                     [&](const QVector<WeatherWarning>& warnings, const QVector<StormReport>& reports,
                         const QDateTime&) {
                         std::cout << "PROBE: snapshot warnings=" << warnings.size()
                                   << " reports=" << reports.size() << '\n';
                     });

    QElapsedTimer wall;
    wall.start();
    source.refresh();
    // Done when the tally line (drawable/partial) arrives and 15 s pass
    // with no further status: progressive zone republishes have settled.
    bool tallied = false;
    while (wall.elapsed() < 240000) {
        application.processEvents(QEventLoop::AllEvents, 200);
        QThread::msleep(200);
        if (!tallied &&
            (lastStatus.contains(QStringLiteral("drawable warnings")) ||
             lastStatus.contains(QStringLiteral("requests failed")))) {
            tallied = true;
        }
        if (tallied && sinceChange.elapsed() > 15000) break;
    }
    std::cout << "PROBE: done tallied=" << (tallied ? 1 : 0)
              << " wall_s=" << (wall.elapsed() / 1000) << " final=" << lastStatus.toStdString() << '\n';
    return tallied ? 0 : 1;
}
