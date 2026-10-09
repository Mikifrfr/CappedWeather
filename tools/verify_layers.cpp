// Layer verification: scripted live run that screenshots the METAR overlay
// and the VAD scan pop-out for human review. No synthetic data: needs a
// live KTLX volume plus reachable aviationweather.gov.
//
// Usage: verify_layers <map.png> <vad.png>   (default radar KTLX)
// Prints metar/tilt counts, saves both screenshots, exits nonzero on any
// timeout so CI-style callers fail closed instead of screenshotting empties.

#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTableWidget>
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
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherLayerVerify"));
    QStandardPaths::setTestModeEnabled(true);
    if (argc != 3) {
        std::cerr << "usage: verify_layers <map.png> <vad.png>\n";
        return 2;
    }

    MainWindow window;
    window.resize(1440, 850);
    window.show();
    // The active panel is the visible canvas (1x1 default): object names
    // do not identify it, and driving a hidden panel screenshots nothing.
    auto canvases = window.findChildren<RadarMapCanvas*>();
    RadarMapCanvas* map = nullptr;
    for (auto* canvas : canvases) {
        if (canvas->isVisible()) {
            map = canvas;
            break;
        }
    }
    if (!map && !canvases.isEmpty()) map = canvases.constFirst();
    auto* station = window.findChild<QComboBox*>(QStringLiteral("stationCombo"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("unifiedStatusLabel"));
    if (!map || !station || !status) {
        std::cerr << "verify could not find required UI controls\n";
        return 3;
    }
    const QString radar = QStringLiteral("KTLX");
    const int radarIndex = station->findData(radar);
    if (radarIndex < 0) {
        std::cerr << "KTLX missing from picker\n";
        return 4;
    }
    station->setCurrentIndex(radarIndex);
    station->activated(radarIndex);
    if (!waitUntil(application, 360000, [&] {
            return map->radarId() == radar &&
                   status->text().contains(QStringLiteral("complete volume"),
                                           Qt::CaseInsensitive);
        })) {
        std::cerr << "volume timeout: " << status->text().toStdString() << '\n';
        return 5;
    }
    // METAR stations follow radar selection; wait for real markers, not the
    // quiet badge.
    if (!waitUntil(application, 180000, [&] { return map->metarCount() > 0; })) {
        std::cerr << "metar timeout: no stations near KTLX\n";
        return 6;
    }
    map->setView(QPointF(-97.5, 35.5), 7);
    waitUntil(application, 8000, [&] { return map->tiltDiagnostics().size() >= 4; });
    application.processEvents();
    std::cout << "view=" << map->viewCenter().x() << ',' << map->viewCenter().y()
              << " zoom=" << map->viewZoom() << '\n';
    const QString mapShot = QString::fromLocal8Bit(argv[1]);
    if (!window.grab().save(mapShot, "PNG")) {
        std::cerr << "could not save map screenshot\n";
        return 7;
    }
    // The scan pop-out is a private slot: invoke it like the Details button.
    if (!QMetaObject::invokeMethod(&window, "showScanDiagnostics", Qt::DirectConnection)) {
        std::cerr << "could not open scan diagnostics\n";
        return 8;
    }
    application.processEvents();
    auto* dialog = window.findChild<QDialog*>();
    auto* table = dialog ? dialog->findChild<QTableWidget*>() : nullptr;
    if (!table || table->rowCount() == 0) {
        std::cerr << "scan dialog has no tilt rows\n";
        return 9;
    }
    const QString vadShot = QString::fromLocal8Bit(argv[2]);
    if (!dialog->grab().save(vadShot, "PNG")) {
        std::cerr << "could not save dialog screenshot\n";
        return 10;
    }
    std::cout << "verify_layers=passed metars=" << map->metarCount()
              << " tilts=" << map->tiltDiagnostics().size()
              << " vad_rows=" << table->rowCount()
              << " map=" << mapShot.toStdString() << " vad=" << vadShot.toStdString() << '\n';
    return 0;
}
