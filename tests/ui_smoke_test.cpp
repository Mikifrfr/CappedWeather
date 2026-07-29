#include "app/main_window.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/weather_overlay_source.hpp"
#include "app/gif_writer.hpp"
#include "app/shortcut_help_dialog.hpp"
#include "core/product.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QImage>
#include <QLabel>
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
    auto* tiltCombo = window.findChild<QComboBox*>(QStringLiteral("tiltCombo"));
    auto* crossSectionButton = window.findChild<QToolButton*>(QStringLiteral("crossSectionButton"));
    auto* frameSlider = window.findChild<QSlider*>(QStringLiteral("frameSlider"));
    auto* frameLabel = window.findChild<QLabel*>(QStringLiteral("frameLabel"));
    auto* playButton = window.findChild<QToolButton*>(QStringLiteral("playButton"));
    auto* snapshotButton = window.findChild<QToolButton*>(QStringLiteral("snapshotButton"));
    auto* timelineButton = window.findChild<QToolButton*>(QStringLiteral("timelineButton"));
    auto* warningsCheck = window.findChild<QCheckBox*>(QStringLiteral("warningsCheck"));
    auto* reportsCheck = window.findChild<QCheckBox*>(QStringLiteral("reportsCheck"));
    auto* lightningCheck = window.findChild<QCheckBox*>(QStringLiteral("lightningCheck"));
    auto* lightningFusionCheck = window.findChild<QCheckBox*>(QStringLiteral("lightningFusionCheck"));
    auto* glmStatusNote = window.findChild<QLabel*>(QStringLiteral("glmStatusNote"));
    auto* belowThresholdCheck = window.findChild<QCheckBox*>(QStringLiteral("belowThresholdCheck"));
    auto* stationCombo = window.findChild<QComboBox*>(QStringLiteral("stationCombo"));
    auto* liveButton = window.findChild<QToolButton*>(QStringLiteral("liveButton"));
    auto* panelLayout = window.findChild<QComboBox*>(QStringLiteral("panelLayoutCombo"));
    auto* controlDock = window.findChild<QDockWidget*>(QStringLiteral("controlDock"));
    const auto radarPanels = window.findChildren<RadarMapCanvas*>();
    auto* loadingProgress = window.findChild<QProgressBar*>();
    auto* model = window.findChild<RadarListModel*>();
    if (!map || !secondMap || !products || products->count() != 8 ||
        products->findData(static_cast<int>(ProductKind::EchoTop)) < 0 ||
        products->findData(static_cast<int>(ProductKind::VerticallyIntegratedLiquid)) < 0 ||
        !tiltCombo || !crossSectionButton || !frameSlider || !frameLabel || !playButton ||
        !snapshotButton || !timelineButton || !warningsCheck || !reportsCheck ||
        !lightningCheck || !lightningFusionCheck ||
        !belowThresholdCheck ||
        !stationCombo || stationCombo->count() < 200 || !liveButton || !loadingProgress ||
        !model || model->rowCount() < 200 || !panelLayout || panelLayout->count() != 4 ||
        !controlDock || controlDock->width() < 220 ||
        radarPanels.size() != 4 || !glmStatusNote) {
        std::cerr << "UI smoke setup failed\n";
        return 1;
    }
    auto* emptyStateHint = window.findChild<QLabel*>(QStringLiteral("emptyStateHint"));
    if (!emptyStateHint || emptyStateHint->text().isEmpty()) {
        std::cerr << "Empty-state hint is missing or empty\n";
        return 1;
    }

    products->setCurrentIndex(products->findData(static_cast<int>(ProductKind::EchoTop)));
    application.processEvents();
    if (map->product() != ProductKind::EchoTop || tiltCombo->isEnabled() ||
        tiltCombo->currentText() != QStringLiteral("All tilts (volume derived)") ||
        crossSectionButton->isEnabled()) {
        std::cerr << "Derived product controls did not switch to all-tilt 2D mode\n";
        return 1;
    }
    products->setCurrentIndex(products->findData(static_cast<int>(ProductKind::Reflectivity)));
    application.processEvents();
    if (!crossSectionButton->isEnabled()) {
        std::cerr << "Cross-section control did not recover for native products\n";
        return 1;
    }

    window.resize(1024, 700);
    application.processEvents();
    const auto belowThresholdBottomRight = belowThresholdCheck->mapTo(
        &window, belowThresholdCheck->rect().bottomRight());
    if (window.width() > 1024 || !window.rect().contains(belowThresholdBottomRight)) {
        std::cerr << "Responsive controls overflow at 1024 px: window=" << window.width()
                  << " controlRight=" << belowThresholdBottomRight.x()
                  << " contentRight=" << window.rect().right() << '\n';
        return 1;
    }
    const auto frameLabelBottomRight = frameLabel->mapTo(&window, frameLabel->rect().bottomRight());
    const auto snapshotTopLeft = snapshotButton->mapTo(&window, snapshotButton->rect().topLeft());
    const auto timelineBottomRight = timelineButton->mapTo(&window, timelineButton->rect().bottomRight());
    if (frameLabelBottomRight.x() >= snapshotTopLeft.x() ||
        !window.rect().contains(timelineBottomRight) ||
        frameLabel->width() < 185) {
        std::cerr << "Playback details overlap export controls: labelRight=" << frameLabelBottomRight.x()
                  << " snapshotLeft=" << snapshotTopLeft.x()
                  << " labelWidth=" << frameLabel->width()
                  << " timelineRight=" << timelineBottomRight.x() << '\n';
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
    panelLayout->setCurrentIndex(1);
    application.processEvents();

    const auto* mapHost = map->parentWidget();
    if (!mapHost || std::abs(map->geometry().bottom() - mapHost->rect().bottom()) > 5 ||
        std::abs(secondMap->geometry().bottom() - mapHost->rect().bottom()) > 5) {
        std::cerr << "1x2 panel layout did not use the full available map height\n";
        return 1;
    }

    secondMap->setProduct(ProductKind::EchoTop);
    secondMap->setCrossSectionMode(true);
    secondMap->activated();
    application.processEvents();
    if (crossSectionButton->isEnabled() || secondMap->crossSectionMode()) {
        std::cerr << "Derived-product panel activation exposed cross-section controls\n";
        return 1;
    }
    const auto secondRadarIndex = stationCombo->findData(QStringLiteral("KTLX"));
    if (secondRadarIndex < 0) {
        std::cerr << "Shared panel radar was not available in the station picker\n";
        return 1;
    }
    stationCombo->setCurrentIndex(secondRadarIndex);
    stationCombo->activated(secondRadarIndex);
    application.processEvents();
    if (secondMap->radarId() != QStringLiteral("KTLX") || map->radarId() != QStringLiteral("KTLX")) {
        std::cerr << "Radar selection was not synchronized across panels\n";
        return 1;
    }
    map->activated();
    application.processEvents();
    if (!crossSectionButton->isEnabled()) {
        std::cerr << "Native-product panel activation did not restore cross-section controls\n";
        return 1;
    }
    panelLayout->setCurrentIndex(0);
    application.processEvents();

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
                      "senderName":"NWS Norman","description":"Observed tornado. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling. Long warning details for smooth scrolling.","instruction":"Take shelter now",
                      "onset":"2026-07-13T19:00:00+00:00","expires":"2026-07-13T20:00:00+00:00"}
      }]})JSON";
    QString parseError;
    const auto warnings = WeatherOverlaySource::parseAlerts(alerts, &parseError);
    if (!parseError.isEmpty() || warnings.size() != 1 || warnings.front().polygons.size() != 1 ||
        warnings.front().polygons.front().holes.size() != 1 || warnings.front().senderName != QStringLiteral("NWS Norman") ||
        warnings.front().alertPriority != 2 ||
        warnings.front().alertColor.name(QColor::HexRgb).compare(QStringLiteral("#ff0000"), Qt::CaseInsensitive) != 0) {
        std::cerr << "NWS alert parsing failed\n";
        return 1;
    }
    if (nwsOfficialAlertStyleCount() != 111 ||
        nwsAlertStyle(QStringLiteral("winter weather advisory")).priority != 60 ||
        nwsAlertStyle(QStringLiteral("Winter Weather Advisory")).color.name(QColor::HexRgb) !=
            QStringLiteral("#7b68ee") ||
        nwsAlertStyle(QStringLiteral("Air Quality Alert")).color.name(QColor::HexRgb) !=
            QStringLiteral("#808080")) {
        std::cerr << "NWS official alert color table is incomplete or incorrect\n";
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
    map->setSites({{"OUTSIDE", "Outside", "WSR-88D", 0.0, 0.0, 0.0}});
    map->setView({-97.2, 35.2}, 6);
    application.processEvents();
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, map->rect().center());
    application.processEvents();
    if (!map->probeText().contains(QLatin1Char('\n')) ||
        !map->probeText().contains(QStringLiteral("Instructions: Take shelter now"))) {
        std::cerr << "Warning detail card lost structured alert text\n";
        return 1;
    }
    const auto initialCardRect = map->infoCardRect();
    const auto dragStart = (initialCardRect.topLeft() + QPointF(90.0, 18.0)).toPoint();
    const auto dragEnd = dragStart + QPoint(-70, 45);
    QTest::mousePress(map, Qt::LeftButton, Qt::NoModifier, dragStart);
    QTest::mouseMove(map, dragEnd);
    QTest::mouseRelease(map, Qt::LeftButton, Qt::NoModifier, dragEnd);
    application.processEvents();
    if (map->infoCardRect().topLeft() == initialCardRect.topLeft()) {
        std::cerr << "Warning detail card could not be dragged by its header\n";
        return 1;
    }
    const auto scrollPosition = map->infoCardRect().center();
    QWheelEvent partialWheel(
        scrollPosition, map->mapToGlobal(scrollPosition.toPoint()),
        QPoint{}, QPoint(0, -30), Qt::NoButton, Qt::NoModifier,
        Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(map, &partialWheel);
    QTest::qWait(80);
    if (map->infoCardScrollOffset() <= 0.0) {
        std::cerr << "Warning detail card discarded high-resolution wheel input\n";
        return 1;
    }
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier, QPoint(8, map->height() - 8));
    application.processEvents();
    if (!map->probeText().isEmpty()) {
        std::cerr << "Warning detail card did not dismiss after clicking outside it\n";
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
    LightningCellSnapshot lightning;
    lightning.kind = JumpEventKind::JumpUpdate;
    lightning.eventTimeEpochSec = QDateTime::currentSecsSinceEpoch() - 45;
    lightning.latitudeCenterDeg = 33.65;
    lightning.longitudeCenterDeg = -83.85;
    lightning.sigmaLevel = 3.2;
    lightning.flashRatePerMin = 28.0;
    lightning.cellX = -336;
    lightning.cellY = 134;
    lightning.satelliteNumber = 19;
    lightning.latestDataEpochSec = QDateTime::currentSecsSinceEpoch() - 12;
    map->setLightningCells({lightning});
    if (map->lightningCellCount() != 1) {
        std::cerr << "GLM lightning overlay did not reach the map\n";
        return 1;
    }
    lightningCheck->setChecked(false);
    application.processEvents();
    if (lightningFusionCheck->isEnabled()) {
        std::cerr << "Fusion control remained enabled while GLM overlay was disabled\n";
        return 1;
    }
    lightningCheck->setChecked(true);
    map->recenterSelected();
    map->setView({-84.2620, 33.6470}, 8);
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
    QTest::mouseClick(map, Qt::LeftButton, Qt::NoModifier,
                      map->lightningCellScreenPoint(0));
    application.processEvents();
    const auto lightningInfo = map->probeText();
    if (!lightningInfo.contains(QStringLiteral("GLM LIGHTNING JUMP")) ||
        !lightningInfo.contains(QStringLiteral("ACTIVE UPDATE")) ||
        !lightningInfo.contains(QStringLiteral("GOES-19 East")) ||
        !lightningInfo.contains(QStringLiteral("Feed: LIVE")) ||
        !lightningInfo.contains(QStringLiteral("Radar fusion:"))) {
        std::cerr << "GLM intelligence inspector incomplete: "
                  << lightningInfo.toStdString() << '\n';
        return 1;
    }

    window.resizeDocks({controlDock}, {386}, Qt::Horizontal);
    application.processEvents();
    const auto savedDockWidth = controlDock->width();
    window.close();
    application.processEvents();
    MainWindow restoredWindow;
    restoredWindow.show();
    application.processEvents();
    const auto* restoredDock = restoredWindow.findChild<QDockWidget*>(QStringLiteral("controlDock"));
    if (!restoredDock || std::abs(restoredDock->width() - savedDockWidth) > 12) {
        std::cerr << "Radar sidebar width was not restored: saved=" << savedDockWidth
                  << " restored=" << (restoredDock ? restoredDock->width() : -1) << '\n';
        return 1;
    }
    restoredWindow.close();

    ShortcutHelpDialog helpDialog;
    if (helpDialog.windowTitle() != QStringLiteral("Keyboard shortcuts — CappedWeather")) {
        std::cerr << "Shortcut help dialog title mismatch\n";
        return 1;
    }

    std::cout << "UI smoke passed\n";
    return 0;
}
