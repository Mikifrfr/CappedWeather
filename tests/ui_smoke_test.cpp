#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/weather_overlay_source.hpp"
#include "core/product.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QTest>
#include <QToolButton>

#include <iostream>

using namespace cappedweather;

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    MainWindow window;
    window.show();
    application.processEvents();

    auto* map = window.findChild<RadarMapCanvas*>(QStringLiteral("radarMap"));
    auto* products = window.findChild<QComboBox*>(QStringLiteral("productCombo"));
    auto* frameSlider = window.findChild<QSlider*>(QStringLiteral("frameSlider"));
    auto* playButton = window.findChild<QToolButton*>(QStringLiteral("playButton"));
    auto* warningsCheck = window.findChild<QCheckBox*>(QStringLiteral("warningsCheck"));
    auto* reportsCheck = window.findChild<QCheckBox*>(QStringLiteral("reportsCheck"));
    auto* model = window.findChild<RadarListModel*>();
    if (!map || !products || !frameSlider || !playButton || !warningsCheck || !reportsCheck ||
        !model || model->rowCount() < 150) {
        std::cerr << "UI smoke setup failed\n";
        return 1;
    }

    map->setSites({{"KTLX", "Oklahoma City, OK", "WSR-88D", 35.3331, -97.2778, 389.0}});
    application.processEvents();
    const auto targetPoint = map->radarScreenPoint(QStringLiteral("KTLX"));
    if (!map->rect().contains(targetPoint)) {
        std::cerr << "Radar pill is not visible\n";
        return 1;
    }
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, targetPoint);
    application.processEvents();
    if (map->radarId() != QStringLiteral("KTLX")) {
        std::cerr << "Radar pill click did not select the requested radar\n";
        return 1;
    }

    products->setCurrentIndex(2);
    application.processEvents();
    if (map->product() != ProductKind::SpectrumWidth) {
        std::cerr << "Product selection did not reach the canvas\n";
        return 1;
    }

    const QByteArray alerts = R"JSON({
      "type":"FeatureCollection","features":[{
        "type":"Feature",
        "geometry":{"type":"Polygon","coordinates":[[[-98.0,35.0],[-97.0,35.0],[-97.0,36.0],[-98.0,35.0]]]},
        "properties":{"event":"Tornado Warning","headline":"Test warning","severity":"Extreme",
                      "expires":"2026-07-13T20:00:00+00:00"}
      }]})JSON";
    QString parseError;
    const auto warnings = WeatherOverlaySource::parseAlerts(alerts, &parseError);
    if (!parseError.isEmpty() || warnings.size() != 1 || warnings.front().polygons.size() != 1) {
        std::cerr << "NWS alert parsing failed\n";
        return 1;
    }
    const QByteArray reports =
        "Time,Size,Location,County,State,Lat,Lon,Comments\n"
        "1830,1.75,Norman,Cleveland,OK,35.22,-97.44,Quarter-size hail\n";
    const auto stormReports = WeatherOverlaySource::parseStormReports(
        reports, StormReportKind::Hail, &parseError);
    if (!parseError.isEmpty() || stormReports.size() != 1 ||
        stormReports.front().kind != StormReportKind::Hail) {
        std::cerr << "SPC report parsing failed\n";
        return 1;
    }
    map->setWeatherOverlays(warnings, stormReports);
    if (map->warningCount() != 1 || map->stormReportCount() != 1) {
        std::cerr << "Weather overlays did not reach the map\n";
        return 1;
    }

    DecodedRay ray;
    ray.azimuthDegrees = 90.0F;
    ray.azimuthResolutionDegrees = 1.0F;
    ray.elevationNumber = 1;
    ray.elevationDegrees = 0.5F;
    const auto reflectivityIndex = static_cast<std::size_t>(ProductKind::Reflectivity);
    ray.fields.at(reflectivityIndex).assign(1000, 86);
    ray.firstGateByProduct.at(reflectivityIndex) = 0;
    ray.gateSpacingByProduct.at(reflectivityIndex) = 250;
    ray.scaleByProduct.at(reflectivityIndex) = 2.0F;
    ray.offsetByProduct.at(reflectivityIndex) = 66.0F;
    map->setProduct(ProductKind::Reflectivity);
    map->setElevationFilter(1);
    map->setDecodedChunk({"KTLX", {ray}, 1}, QDateTime::currentDateTimeUtc());
    QTest::mouseMove(map, map->radarScreenPoint(QStringLiteral("KTLX")) + QPoint(20, 0));
    application.processEvents();
    if (map->probeText().isEmpty() || !map->probeText().contains(QStringLiteral("dBZ"))) {
        std::cerr << "Radar cursor probe failed\n";
        return 1;
    }

    std::cout << "UI smoke passed\n";
    return 0;
}
