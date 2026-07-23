#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"
#include "core/radar_catalog.hpp"
#include "core/radar_geometry.hpp"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QStandardPaths>
#include <QThread>

#include <cmath>
#include <iostream>
#include <limits>

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

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherLiveProbe"));
    QStandardPaths::setTestModeEnabled(true);
    if (argc != 2) {
        std::cerr << "usage: cappedweather_live_gui_probe <screenshot.png>\n";
        return 2;
    }

    MainWindow window;
    window.resize(1440, 850);
    window.show();
    auto* map = window.findChild<RadarMapCanvas*>(QStringLiteral("radarMap"));
    auto* station = window.findChild<QComboBox*>(QStringLiteral("stationCombo"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("unifiedStatusLabel"));
    if (!map || !station || !status) {
        std::cerr << "live probe could not find required UI controls\n";
        return 3;
    }

    const auto sites = RadarCatalog::defaultSites();
    const RadarSite* nearest = nullptr;
    double latitude = 0.0;
    double longitude = 0.0;
    double nearestMeters = std::numeric_limits<double>::infinity();
    const bool gotRadarReachableLightning = waitUntil(application, 180000, [&] {
        nearest = nullptr;
        nearestMeters = std::numeric_limits<double>::infinity();
        for (const auto& cell : map->lightningCells()) {
            if (!cell.active()) continue;
            for (const auto& site : sites) {
                if (!site.latitude || !site.longitude) continue;
                const auto distance = rangeAndBearing(
                    {cell.latitudeCenterDeg, cell.longitudeCenterDeg},
                    {*site.latitude, *site.longitude}).distanceMeters;
                if (distance < nearestMeters) {
                    nearestMeters = distance;
                    nearest = &site;
                    latitude = cell.latitudeCenterDeg;
                    longitude = cell.longitudeCenterDeg;
                }
            }
        }
        return nearest && nearestMeters <= 300000.0;
    });
    if (!gotRadarReachableLightning) {
        std::cerr << "live GLM timeout: no active jump within 300 km of a radar; "
                  << status->text().toStdString() << '\n';
        return 4;
    }
    const auto radarId = QString::fromStdString(nearest->id);
    const auto radarIndex = station->findData(radarId);
    if (radarIndex < 0) {
        std::cerr << "nearest radar missing from picker: " << nearest->id << '\n';
        return 6;
    }
    station->setCurrentIndex(radarIndex);
    station->activated(radarIndex);
    map->setView({*nearest->longitude, *nearest->latitude}, 7);

    const bool gotRadar = waitUntil(application, 180000, [&] {
        return map->radarId() == radarId && status->text().contains(
            QStringLiteral("received 1 complete volume"), Qt::CaseInsensitive);
    });
    if (!gotRadar) {
        std::cerr << "live radar timeout for " << nearest->id << ": "
                  << status->text().toStdString() << '\n';
        return 7;
    }

    waitUntil(application, 5000, [&] { return map->radarFeatureCount() > 0; });
    application.processEvents();
    const QString output = QString::fromLocal8Bit(argv[1]);
    if (!window.grab().save(output, "PNG") || !QFileInfo(output).exists()) {
        std::cerr << "could not save live screenshot\n";
        return 8;
    }
    std::cout << "live_probe=passed radar=" << nearest->id
              << " jump=" << latitude << ',' << longitude
              << " distance_km=" << nearestMeters / 1000.0
              << " lightning_cells=" << map->lightningCellCount()
              << " radar_features=" << map->radarFeatureCount()
              << " fusion_links=" << map->lightningFusionCount()
              << " screenshot=" << output.toStdString() << '\n'
              << "status=" << status->text().toStdString() << '\n';
    return 0;
}
