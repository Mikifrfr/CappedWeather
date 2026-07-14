#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/weather_overlay_source.hpp"
#include "app/gif_writer.hpp"
#include "core/product.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QImage>
#include <QSlider>
#include <QProgressBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTest>
#include <QTemporaryDir>
#include <QToolButton>

#include <iostream>

using namespace cappedweather;

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("CappedWeatherTests"));
    application.setApplicationName(QStringLiteral("CappedWeatherUiSmoke"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    MainWindow window;
    window.show();
    application.processEvents();

    auto* map = window.findChild<RadarMapCanvas*>(QStringLiteral("radarMap"));
    auto* secondMap = window.findChild<RadarMapCanvas*>(QStringLiteral("radarMapPanel2"));
    auto* products = window.findChild<QComboBox*>(QStringLiteral("productCombo"));
    auto* frameSlider = window.findChild<QSlider*>(QStringLiteral("frameSlider"));
    auto* playButton = window.findChild<QToolButton*>(QStringLiteral("playButton"));
    auto* warningsCheck = window.findChild<QCheckBox*>(QStringLiteral("warningsCheck"));
    auto* reportsCheck = window.findChild<QCheckBox*>(QStringLiteral("reportsCheck"));
    auto* stationCombo = window.findChild<QComboBox*>(QStringLiteral("stationCombo"));
    auto* liveButton = window.findChild<QToolButton*>(QStringLiteral("liveButton"));
    auto* panelLayout = window.findChild<QComboBox*>(QStringLiteral("panelLayoutCombo"));
    const auto radarPanels = window.findChildren<RadarMapCanvas*>();
    auto* loadingProgress = window.findChild<QProgressBar*>();
    auto* model = window.findChild<RadarListModel*>();
    if (!map || !secondMap || !products || !frameSlider || !playButton || !warningsCheck || !reportsCheck ||
        !stationCombo || stationCombo->count() < 200 || !liveButton || !loadingProgress ||
        !model || model->rowCount() < 200 || !panelLayout || panelLayout->count() != 4 ||
        radarPanels.size() != 4) {
        std::cerr << "UI smoke setup failed\n";
        return 1;
    }

    panelLayout->setCurrentIndex(3);
    application.processEvents();
    const auto visiblePanels = std::count_if(radarPanels.cbegin(), radarPanels.cend(),
                                             [](const RadarMapCanvas* panel) { return panel->isVisible(); });
    if (visiblePanels != 4) {
        std::cerr << "2x2 panel layout did not expose four radar canvases\n";
        return 1;
    }
    panelLayout->setCurrentIndex(0);
    application.processEvents();

    secondMap->activated();
    const auto secondRadarIndex = stationCombo->findData(QStringLiteral("KTLX"));
    if (secondRadarIndex < 0) {
        std::cerr << "Independent panel radar was not available in the station picker\n";
        return 1;
    }
    stationCombo->setCurrentIndex(secondRadarIndex);
    stationCombo->activated(secondRadarIndex);
    application.processEvents();
    if (secondMap->radarId() != QStringLiteral("KTLX") || map->radarId() == QStringLiteral("KTLX")) {
        std::cerr << "Radar selection was not independent per panel\n";
        return 1;
    }
    map->activated();

    map->setSites({{"TATL", "Atlanta", "TDWR", 33.6470, -84.2620, 327.7}});
    map->setView({-84.2620, 33.6470}, 6);
    application.processEvents();
    const auto targetPoint = map->radarScreenPoint(QStringLiteral("TATL"));
    if (!map->rect().contains(targetPoint)) {
        std::cerr << "Radar pill is not visible: point=" << targetPoint.x() << ',' << targetPoint.y()
                  << " rect=" << map->rect().width() << 'x' << map->rect().height()
                  << " visible=" << map->isVisible() << '\n';
        return 1;
    }
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, targetPoint);
    application.processEvents();
    if (map->radarId() != QStringLiteral("TATL")) {
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
        "geometry":{"type":"Polygon","coordinates":[[[-98.0,35.0],[-97.0,35.0],[-97.0,36.0],[-98.0,35.0]],
                                                               [[-97.8,35.1],[-97.5,35.1],[-97.6,35.3],[-97.8,35.1]]]},
        "properties":{"event":"Tornado Warning","headline":"Test warning","severity":"Extreme",
                      "senderName":"NWS Norman","description":"Observed tornado","instruction":"Take shelter now",
                      "onset":"2026-07-13T19:00:00+00:00","expires":"2026-07-13T20:00:00+00:00"}
      }]})JSON";
    QString parseError;
    const auto warnings = WeatherOverlaySource::parseAlerts(alerts, &parseError);
    if (!parseError.isEmpty() || warnings.size() != 1 || warnings.front().polygons.size() != 1 ||
        warnings.front().polygons.front().holes.size() != 1 || warnings.front().senderName != QStringLiteral("NWS Norman")) {
        std::cerr << "NWS alert parsing failed\n";
        return 1;
    }
    const QByteArray zone = R"JSON({"type":"Feature","geometry":{"type":"MultiPolygon","coordinates":[
        [[[-98.0,35.0],[-97.0,35.0],[-97.0,36.0],[-98.0,35.0]]]
    ]}})JSON";
    const auto zonePolygons = WeatherOverlaySource::parseZone(zone, &parseError);
    if (!parseError.isEmpty() || zonePolygons.size() != 1 || zonePolygons.front().exterior.size() != 4) {
        std::cerr << "NWS zone geometry parsing failed\n";
        return 1;
    }
    const QByteArray reports =
        "Time,Size,Location,County,State,Lat,Lon,Comments\n"
        "1830,1.75,Norman,Cleveland,OK,35.22,-97.44,Quarter-size hail\n";
    const auto stormReports = WeatherOverlaySource::parseStormReports(
        reports, StormReportKind::Hail, &parseError, QDate(2026, 7, 13));
    if (!parseError.isEmpty() || stormReports.size() != 1 ||
        stormReports.front().kind != StormReportKind::Hail ||
        stormReports.front().occurredAt != QDateTime(QDate(2026, 7, 13), QTime(18, 30), QTimeZone::UTC)) {
        std::cerr << "SPC report parsing failed\n";
        return 1;
    }
    const auto noReports = WeatherOverlaySource::parseStormReports(
        QByteArray{}, StormReportKind::Tornado, &parseError);
    if (!parseError.isEmpty() || !noReports.isEmpty()) {
        std::cerr << "Empty SPC report response should be a valid zero-report result\n";
        return 1;
    }
    map->setWeatherOverlays(warnings, stormReports);
    if (map->warningCount() != 1 || map->stormReportCount() != 1) {
        std::cerr << "Weather overlays did not reach the map\n";
        return 1;
    }
    map->setCustomPalette({{-10.0, QColor("#001122")}, {40.0, QColor("#ffcc00")}});
    map->setPalette(RadarMapCanvas::PaletteKind::Custom);
    if (map->customPalette().size() != 2 ||
        RadarMapCanvas::colorForValue(ProductKind::Reflectivity, 45.0,
                                      RadarMapCanvas::PaletteKind::Custom, 100,
                                      map->customPalette()).name() != QStringLiteral("#ffcc00")) {
        std::cerr << "Custom palette application failed\n";
        return 1;
    }
    QTemporaryDir animationDirectory;
    const auto animationPath = animationDirectory.filePath(QStringLiteral("timeline.gif"));
    QImage firstFrame(8, 6, QImage::Format_RGB32);
    QImage secondFrame(8, 6, QImage::Format_RGB32);
    firstFrame.fill(QColor("#ff0000"));
    secondFrame.fill(QColor("#0033ff"));
    if (!writeAnimatedGif(animationPath, {firstFrame, secondFrame}, 10, &parseError)) {
        std::cerr << "Animated GIF export failed\n";
        return 1;
    }
    QFile animation(animationPath);
    if (!animation.open(QIODevice::ReadOnly)) return 1;
    const auto animationBytes = animation.readAll();
    if (!animationBytes.startsWith("GIF89a") || !animationBytes.contains("NETSCAPE2.0") ||
        animationBytes.back() != ';' || QImage(animationPath).isNull()) {
        std::cerr << "Animated GIF output is invalid\n";
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
    map->setSites({{"TATL", "Atlanta", "TDWR", 33.6470, -84.2620, 327.7}});
    map->setSelectedRadar(QStringLiteral("TATL"));
    map->setProduct(ProductKind::Reflectivity);
    map->setElevationFilter(1);
    map->setDecodedChunk({"TATL", {ray}, 1}, QDateTime::currentDateTimeUtc());
    map->recenterSelected();
    map->setVolumeQuality(9, 10, 1);
    map->setPalette(RadarMapCanvas::PaletteKind::Operational);
    map->setRadarOpacity(65);
    map->setBelowThresholdVisible(true);
    const auto probePoint = map->radarScreenPoint(QStringLiteral("TATL")) + QPoint(20, 0);
    map->inspectAt(QPointF(probePoint));
    if (map->probeText().isEmpty() || !map->probeText().contains(QStringLiteral("dBZ"))) {
        const auto radarPoint = map->radarScreenPoint(QStringLiteral("TATL"));
        std::cerr << "Radar cursor probe failed: " << map->probeText().toStdString()
                  << " radar=" << map->radarId().toStdString()
                  << " point=" << radarPoint.x() << ',' << radarPoint.y()
                  << " size=" << map->width() << 'x' << map->height() << "\n";
        return 1;
    }

    std::cout << "UI smoke passed\n";
    return 0;
}
