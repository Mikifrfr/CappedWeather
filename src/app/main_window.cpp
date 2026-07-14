#include "app/main_window.hpp"

#include "app/nexrad_source.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/weather_overlay_source.hpp"
#include "app/cross_section_widget.hpp"
#include "app/gif_writer.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"

#include <QComboBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCompleter>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QScreen>
#include <QSlider>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <QThreadPool>
#include <QToolButton>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtMath>

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <span>
#include <optional>
#include <vector>

namespace cappedweather {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      model_(new RadarListModel(this)),
      source_(new NexradSource(this)),
      overlaySource_(new WeatherOverlaySource(this)),
      refreshTimer_(new QTimer(this)),
      overlayRefreshTimer_(new QTimer(this)),
      animationTimer_(new QTimer(this)) {
    buildUi();

    QSettings settings;
    const auto storedFavorites = settings.value(QStringLiteral("favoriteRadars")).toStringList();
    for (const auto& favorite : storedFavorites) favoriteRadarIds_.insert(favorite);
    recentRadarIds_ = settings.value(QStringLiteral("recentRadars")).toStringList();
    rebuildStationPicker();

    for (auto* panel : maps_) {
        connect(panel, &RadarMapCanvas::radarClicked, this, [this](const QString& radarId) {
            const auto row = model_->rowForRadar(radarId);
            if (row >= 0) radarSelected(model_->index(row, 0));
        });
        connect(panel, &RadarMapCanvas::activated, this, [this, panel] { activateMap(panel); });
        connect(panel, &RadarMapCanvas::viewChanged, this,
                [this, panel](const QPointF&, int) { synchronizeMapView(panel); });
        connect(panel, &RadarMapCanvas::crossSectionRequested, this,
                [this, panel](const QPointF& start, const QPointF& end) {
                    activateMap(panel);
                    const QSignalBlocker blocker(crossSectionButton_);
                    crossSectionButton_->setChecked(false);
                    panel->setCrossSectionMode(false);
                    showCrossSection(start, end);
                });
    }
    connect(stationCombo_, qOverload<int>(&QComboBox::activated), this, [this](const int index) {
        const auto radarId = stationCombo_->itemData(index).toString();
        const auto row = model_->rowForRadar(radarId);
        if (row >= 0) radarSelected(model_->index(row, 0));
    });
    connect(stationCombo_->lineEdit(), &QLineEdit::returnPressed, this, [this] {
        const auto query = stationCombo_->currentText().trimmed();
        for (int index = 0; index < stationCombo_->count(); ++index) {
            if (!stationCombo_->itemText(index).contains(query, Qt::CaseInsensitive) &&
                stationCombo_->itemData(index).toString().compare(query, Qt::CaseInsensitive) != 0) continue;
            const auto row = model_->rowForRadar(stationCombo_->itemData(index).toString());
            if (row >= 0) radarSelected(model_->index(row, 0));
            break;
        }
    });
    connect(favoriteButton_, &QToolButton::toggled, this, [this](const bool favorite) {
        if (selectedRadarId_.isEmpty()) return;
        if (favorite) favoriteRadarIds_.insert(selectedRadarId_);
        else favoriteRadarIds_.remove(selectedRadarId_);
        favoriteButton_->setText(favorite ? QStringLiteral("★") : QStringLiteral("☆"));
        QSettings().setValue(QStringLiteral("favoriteRadars"), favoriteRadarIds_.values());
        rebuildStationPicker();
    });
    connect(productCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](const int index) {
        map_->setProduct(static_cast<ProductKind>(productCombo_->itemData(index).toInt()));
        rebuildTiltCombo();
        updateVelocityControls();
    });
    connect(tiltCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](const int index) {
        if (index >= 0 && tiltCombo_->itemData(index).isValid()) {
            map_->setElevationFilter(tiltCombo_->itemData(index).toInt());
        }
    });
    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::requestSelectedRadar);
    connect(overlayRefreshTimer_, &QTimer::timeout, overlaySource_, &WeatherOverlaySource::refresh);
    connect(animationTimer_, &QTimer::timeout, this, &MainWindow::advanceFrame);
    connect(frameSlider_, &QSlider::valueChanged, this, [this](const int index) {
        const auto session = activeSession();
        if (!session) return;
        session->followLatest = index == static_cast<int>(session->history.size()) - 1 && liveButton_->isChecked();
        showFrame(index);
    });
    connect(previousButton_, &QToolButton::clicked, this, [this] {
        const auto session = activeSession();
        if (!session) return;
        stopAnimation();
        session->followLatest = false;
        showFrame(std::max(0, session->frameIndex - 1));
    });
    connect(nextButton_, &QToolButton::clicked, this, [this] {
        const auto session = activeSession();
        if (!session) return;
        stopAnimation();
        session->followLatest = session->frameIndex + 1 >= static_cast<int>(session->history.size()) - 1 &&
                                liveButton_->isChecked();
        showFrame(std::min(static_cast<int>(session->history.size()) - 1, session->frameIndex + 1));
    });
    connect(playButton_, &QToolButton::toggled, this, [this](const bool playing) {
        const auto session = activeSession();
        if (!session) return;
        if (!playing || session->history.size() < 2) {
            if (playing) ensureHistoryLoaded(true);
            stopAnimation();
            return;
        }
        session->followLatest = false;
        liveButton_->setChecked(false);
        if (session->frameIndex >= static_cast<int>(session->history.size()) - 1) showFrame(0);
        playButton_->setText(QStringLiteral("Pause"));
        animationTimer_->start(speedCombo_->currentData().toInt());
    });
    connect(liveButton_, &QToolButton::toggled, this, [this](const bool live) {
        const auto session = activeSession();
        if (!session) return;
        session->followLatest = live;
        if (live && !session->history.empty()) showFrame(static_cast<int>(session->history.size()) - 1);
    });
    connect(speedCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (animationTimer_->isActive()) animationTimer_->setInterval(speedCombo_->currentData().toInt());
    });
    connect(warningsCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setWarningsVisible(visible);
    });
    connect(reportsCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setStormReportsVisible(visible);
    });
    connect(source_, &NexradSource::statusChanged, this, &MainWindow::sourceStatusChanged);
    connect(source_, &NexradSource::requestFailed, this, &MainWindow::sourceFailed);
    connect(source_, &NexradSource::catalogReceived, this, &MainWindow::catalogReceived);
    connect(source_, &NexradSource::volumeStarted, this, &MainWindow::volumeStarted);
    connect(source_, &NexradSource::volumeProgress, this, &MainWindow::volumeProgress);
    connect(source_, &NexradSource::chunkDecompressed, this, &MainWindow::chunkDecompressed);
    connect(source_, &NexradSource::volumeReady, this, &MainWindow::volumeReady);
    connect(overlaySource_, &WeatherOverlaySource::overlaysReady, this,
            [this](QVector<WeatherWarning> warnings, QVector<StormReport> reports, const QDateTime&) {
                for (auto* panel : maps_) panel->setWeatherOverlays(warnings, reports);
            });
    connect(overlaySource_, &WeatherOverlaySource::statusChanged, this, [this](const QString& status) {
        overlayStatus_ = status;
        updateUnifiedStatus();
    });

    connect(retryButton_, &QToolButton::clicked, this, [this] {
        const auto session = activeSession();
        if (!session) return;
        retryButton_->hide();
        session->requestActive = true;
        if (session->historyRequested) {
            session->historyLoadStarted = true;
            source_->requestRecent(selectedRadarId_, historyVolumeCount_);
        } else {
            source_->requestLatest(selectedRadarId_);
        }
    });
    connect(cancelButton_, &QToolButton::clicked, this, [this] {
        const auto session = activeSession();
        source_->cancel(selectedRadarId_);
        if (session) {
            session->requestActive = false;
            session->playWhenHistoryReady = false;
        }
        for (auto* panel : maps_) panel->setLoading(false);
        loadingProgress_->hide();
        cancelButton_->hide();
    });

    const auto addShortcut = [this](const QKeySequence& key, auto action) {
        auto* shortcut = new QShortcut(key, this);
        connect(shortcut, &QShortcut::activated, this, action);
    };
    addShortcut(QKeySequence(Qt::Key_Space), [this] { if (playButton_->isEnabled()) playButton_->toggle(); });
    addShortcut(QKeySequence(Qt::Key_Left), [this] { previousButton_->click(); });
    addShortcut(QKeySequence(Qt::Key_Right), [this] { nextButton_->click(); });
    addShortcut(QKeySequence(Qt::Key_PageUp), [this] {
        tiltCombo_->setCurrentIndex(std::max(0, tiltCombo_->currentIndex() - 1));
    });
    addShortcut(QKeySequence(Qt::Key_PageDown), [this] {
        tiltCombo_->setCurrentIndex(std::min(tiltCombo_->count() - 1, tiltCombo_->currentIndex() + 1));
    });
    addShortcut(QKeySequence(Qt::Key_Plus), [this] { map_->zoomIn(); });
    addShortcut(QKeySequence(Qt::Key_Minus), [this] { map_->zoomOut(); });
    addShortcut(QKeySequence(Qt::Key_R), [this] { map_->recenterSelected(); });
    addShortcut(QKeySequence(Qt::Key_L), [this] { liveButton_->setChecked(true); });
    for (int product = 0; product < productCombo_->count() && product < 9; ++product) {
        addShortcut(QKeySequence(Qt::Key_1 + product), [this, product] { productCombo_->setCurrentIndex(product); });
    }

    restoreSettings();

    for (auto* panel : maps_) panel->setSites(model_->sites());
    if (model_->rowCount() > 0) {
        const auto primaryRadar = !panelRadarIds_.value(0).isEmpty()
                                      ? panelRadarIds_.value(0)
                                      : settings.value(QStringLiteral("lastRadar")).toString();
        auto row = chaseMode_ ? -1 : model_->rowForRadar(primaryRadar);
        if (row < 0) row = 0;
        radarSelected(model_->index(row, 0));
        if (chaseMode_) selectNearestChaseRadar();
        const auto fallbackRadar = map_->radarId();
        for (int panel = 1; panel < maps_.size(); ++panel) {
            const auto radar = panelRadarIds_.value(panel).isEmpty() ? fallbackRadar : panelRadarIds_.value(panel);
            const auto panelRow = model_->rowForRadar(radar);
            if (panelRow < 0) continue;
            activateMap(maps_.at(panel));
            radarSelected(model_->index(panelRow, 0));
        }
        activateMap(maps_.front());
    }
    source_->requestCatalog();
    refreshTimer_->start(std::clamp(settings.value(QStringLiteral("network/radarRefreshSeconds"), 60).toInt(),
                                    30, 600) * 1000);
    overlaySource_->refresh();
    overlayRefreshTimer_->start(std::clamp(settings.value(QStringLiteral("network/overlayRefreshSeconds"), 60).toInt(),
                                           30, 600) * 1000);
    if (!restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray())) {
        if (const auto* screen = QGuiApplication::primaryScreen()) {
            const auto available = screen->availableGeometry();
            const auto width = std::clamp(static_cast<int>(available.width() * 0.78), 900, 1180);
            const auto height = std::clamp(static_cast<int>(available.height() * 0.82), 620, 760);
            resize(std::min(width, available.width()), std::min(height, available.height()));
        } else {
            resize(1024, 680);
        }
    }
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("CappedWeather - Level II Radar"));
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget { background: #0e1216; color: #d7dce0; }"
        "QComboBox, QToolButton, QLineEdit { background: #171d22; border: 1px solid #36424a; padding: 5px; }"
        "QToolButton:checked { background: #174f69; border-color: #80d3ff; }"
        "QLabel#section { color: #9aabb5; font-size: 11px; font-weight: 600; }"
        "QProgressBar { border: 1px solid #36424a; background: #171d22; height: 8px; }"
        "QProgressBar::chunk { background: #40a9d5; }"));

    auto* root = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(10);

    auto* controls = new QHBoxLayout();
    controls->setSpacing(6);

    stationCombo_ = new QComboBox(root);
    stationCombo_->setObjectName(QStringLiteral("stationCombo"));
    stationCombo_->setEditable(true);
    stationCombo_->setInsertPolicy(QComboBox::NoInsert);
    stationCombo_->setMinimumWidth(230);
    stationCombo_->lineEdit()->setPlaceholderText(QStringLiteral("Find radar, city, or state…"));
    controls->addWidget(stationCombo_);
    favoriteButton_ = new QToolButton(root);
    favoriteButton_->setObjectName(QStringLiteral("favoriteRadarButton"));
    favoriteButton_->setText(QStringLiteral("☆"));
    favoriteButton_->setCheckable(true);
    favoriteButton_->setToolTip(QStringLiteral("Favorite selected radar"));
    controls->addWidget(favoriteButton_);

    productCombo_ = new QComboBox(root);
    productCombo_->setObjectName(QStringLiteral("productCombo"));
    for (const auto& product : productDescriptors()) {
        productCombo_->addItem(QString::fromUtf8(product.label.data()), static_cast<int>(product.kind));
    }
    controls->addWidget(productCombo_, 0);

    tiltCombo_ = new QComboBox(root);
    tiltCombo_->setObjectName(QStringLiteral("tiltCombo"));
    tiltCombo_->addItem(QStringLiteral("Latest available tilt"));
    tiltCombo_->setEnabled(false);
    controls->addWidget(tiltCombo_, 0);

    stationSummaryLabel_ = new QLabel(QStringLiteral("Select a radar"), root);
    stationSummaryLabel_->setObjectName(QStringLiteral("section"));
    stationSummaryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    controls->addWidget(stationSummaryLabel_, 1);

    statusLabel_ = new QLabel(root);
    statusLabel_->setObjectName(QStringLiteral("section"));
    statusLabel_->hide();
    warningsCheck_ = new QCheckBox(QStringLiteral("Warnings"), root);
    warningsCheck_->setObjectName(QStringLiteral("warningsCheck"));
    warningsCheck_->setChecked(true);
    controls->addWidget(warningsCheck_);
    reportsCheck_ = new QCheckBox(QStringLiteral("Storm reports"), root);
    reportsCheck_->setObjectName(QStringLiteral("reportsCheck"));
    reportsCheck_->setChecked(true);
    controls->addWidget(reportsCheck_);
    rootLayout->addLayout(controls);

    auto* mapHost = new QWidget(root);
    mapGrid_ = new QGridLayout(mapHost);
    mapGrid_->setContentsMargins(0, 0, 0, 0);
    mapGrid_->setSpacing(4);
    for (int panel = 0; panel < 4; ++panel) {
        auto* canvas = new RadarMapCanvas(mapHost);
        canvas->setObjectName(panel == 0 ? QStringLiteral("radarMap")
                                        : QStringLiteral("radarMapPanel%1").arg(panel + 1));
        maps_.push_back(canvas);
        panelRadarIds_.push_back({});
    }
    maps_.at(0)->setProduct(ProductKind::Reflectivity);
    maps_.at(1)->setProduct(ProductKind::Velocity);
    maps_.at(2)->setProduct(ProductKind::CorrelationCoefficient);
    maps_.at(3)->setProduct(ProductKind::DifferentialReflectivity);
    map_ = maps_.front();
    map_->setActivePanel(true);
    setPanelLayout(0);

    auto* viewControls = new QHBoxLayout();
    viewControls->setSpacing(6);
    const auto addMapButton = [root, viewControls](const QString& text, const QString& tip) {
        auto* button = new QToolButton(root);
        button->setText(text);
        button->setToolTip(tip);
        viewControls->addWidget(button);
        return button;
    };
    auto* nationalButton = addMapButton(QStringLiteral("Home"), QStringLiteral("National view"));
    auto* recenterButton = addMapButton(QStringLiteral("Recenter"), QStringLiteral("Center selected radar (R)"));
    auto* zoomOutButton = addMapButton(QStringLiteral("−"), QStringLiteral("Zoom out (-)"));
    auto* zoomInButton = addMapButton(QStringLiteral("+"), QStringLiteral("Zoom in (+)"));
    connect(nationalButton, &QToolButton::clicked, this, [this] { map_->showNationalView(); });
    connect(recenterButton, &QToolButton::clicked, this, [this] { map_->recenterSelected(); });
    connect(zoomOutButton, &QToolButton::clicked, this, [this] { map_->zoomOut(); });
    connect(zoomInButton, &QToolButton::clicked, this, [this] { map_->zoomIn(); });

    measureButton_ = addMapButton(QStringLiteral("Measure"), QStringLiteral("Measure distance and bearing"));
    measureButton_->setCheckable(true);
    connect(measureButton_, &QToolButton::toggled, this, [this](bool enabled) {
        if (enabled && crossSectionButton_) crossSectionButton_->setChecked(false);
        map_->setMeasureMode(enabled);
    });
    crossSectionButton_ = addMapButton(QStringLiteral("Cross-section"),
                                       QStringLiteral("Draw a vertical cross-section through all tilts"));
    crossSectionButton_->setCheckable(true);
    connect(crossSectionButton_, &QToolButton::toggled, this, [this](bool enabled) {
        if (enabled) measureButton_->setChecked(false);
        map_->setCrossSectionMode(enabled);
    });
    auto* settingsButton = addMapButton(QStringLiteral("Settings"),
                                        QStringLiteral("Operational and chase settings"));
    connect(settingsButton, &QToolButton::clicked, this, &MainWindow::showSettingsDialog);

    viewControls->addSpacing(8);
    warningFilterCombo_ = new QComboBox(root);
    warningFilterCombo_->addItems({QStringLiteral("All warnings"), QStringLiteral("Tornado"),
                                   QStringLiteral("Severe Thunderstorm"), QStringLiteral("Flash Flood"),
                                   QStringLiteral("Extreme")});
    warningFilterCombo_->setToolTip(QStringLiteral("Filter visible warning polygons"));
    viewControls->addWidget(warningFilterCombo_);
    connect(warningFilterCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        for (auto* panel : maps_)
            panel->setWarningFilter(index == 0 ? QString{} : warningFilterCombo_->itemText(index));
    });
    tornadoCheck_ = new QCheckBox(QStringLiteral("Tornado"), root);
    hailCheck_ = new QCheckBox(QStringLiteral("Hail"), root);
    windCheck_ = new QCheckBox(QStringLiteral("Wind"), root);
    for (auto* check : {tornadoCheck_, hailCheck_, windCheck_}) check->setChecked(true);
    viewControls->addWidget(tornadoCheck_);
    viewControls->addWidget(hailCheck_);
    viewControls->addWidget(windCheck_);
    connect(tornadoCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setStormReportKindVisible(StormReportKind::Tornado, visible);
    });
    connect(hailCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setStormReportKindVisible(StormReportKind::Hail, visible);
    });
    connect(windCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setStormReportKindVisible(StormReportKind::Wind, visible);
    });

    viewControls->addStretch(1);
    velocityModeCombo_ = new QComboBox(root);
    velocityModeCombo_->setObjectName(QStringLiteral("velocityModeCombo"));
    velocityModeCombo_->addItems({QStringLiteral("Native velocity"), QStringLiteral("Dealiased velocity"),
                                  QStringLiteral("Storm-relative velocity")});
    velocityModeCombo_->setToolTip(QStringLiteral("Velocity processing for the active panel"));
    viewControls->addWidget(velocityModeCombo_);
    nyquistSpin_ = new QDoubleSpinBox(root);
    nyquistSpin_->setRange(1.0, 80.0);
    nyquistSpin_->setValue(25.0);
    nyquistSpin_->setSuffix(QStringLiteral(" m/s Nyq"));
    nyquistSpin_->setDecimals(1);
    nyquistSpin_->setToolTip(QStringLiteral("Nyquist velocity used by optional dealiasing"));
    viewControls->addWidget(nyquistSpin_);
    stormDirectionSpin_ = new QDoubleSpinBox(root);
    stormDirectionSpin_->setRange(0.0, 359.9);
    stormDirectionSpin_->setSuffix(QStringLiteral(" deg motion"));
    stormDirectionSpin_->setDecimals(1);
    stormDirectionSpin_->setToolTip(QStringLiteral("Direction toward which the storm is moving"));
    viewControls->addWidget(stormDirectionSpin_);
    stormSpeedSpin_ = new QDoubleSpinBox(root);
    stormSpeedSpin_->setRange(0.0, 80.0);
    stormSpeedSpin_->setSuffix(QStringLiteral(" m/s storm"));
    stormSpeedSpin_->setDecimals(1);
    viewControls->addWidget(stormSpeedSpin_);
    const auto applyVelocitySettings = [this] {
        map_->setVelocityProcessing({
            static_cast<VelocityProcessingMode>(velocityModeCombo_->currentIndex()),
            nyquistSpin_->value(), stormDirectionSpin_->value(), stormSpeedSpin_->value(),
        });
        updateVelocityControls();
    };
    connect(velocityModeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [applyVelocitySettings](int) { applyVelocitySettings(); });
    connect(nyquistSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [applyVelocitySettings](double) { applyVelocitySettings(); });
    connect(stormDirectionSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [applyVelocitySettings](double) { applyVelocitySettings(); });
    connect(stormSpeedSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [applyVelocitySettings](double) { applyVelocitySettings(); });
    panelLayoutCombo_ = new QComboBox(root);
    panelLayoutCombo_->setObjectName(QStringLiteral("panelLayoutCombo"));
    panelLayoutCombo_->addItems({QStringLiteral("1x1"), QStringLiteral("1x2"),
                                 QStringLiteral("2x1"), QStringLiteral("2x2")});
    panelLayoutCombo_->setToolTip(QStringLiteral("Radar panel layout"));
    viewControls->addWidget(panelLayoutCombo_);
    connect(panelLayoutCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::setPanelLayout);
    paletteCombo_ = new QComboBox(root);
    paletteCombo_->addItems({QStringLiteral("Capped palette"), QStringLiteral("Native operational palette"),
                             QStringLiteral("Color-safe palette"), QStringLiteral("Custom palette")});
    viewControls->addWidget(paletteCombo_);
    connect(paletteCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        for (auto* panel : maps_) panel->setPalette(static_cast<RadarMapCanvas::PaletteKind>(index));
    });
    auto* paletteButton = new QToolButton(root);
    paletteButton->setText(QStringLiteral("Palette..."));
    paletteButton->setToolTip(QStringLiteral("Import a value,color palette text file"));
    connect(paletteButton, &QToolButton::clicked, this, &MainWindow::importCustomPalette);
    viewControls->addWidget(paletteButton);
    auto* snapshotButton = new QToolButton(root);
    snapshotButton->setText(QStringLiteral("Snapshot"));
    connect(snapshotButton, &QToolButton::clicked, this, &MainWindow::exportSnapshot);
    viewControls->addWidget(snapshotButton);
    auto* timelineButton = new QToolButton(root);
    timelineButton->setText(QStringLiteral("Timeline GIF"));
    timelineButton->setToolTip(QStringLiteral("Export loaded volume frames as an animated GIF"));
    connect(timelineButton, &QToolButton::clicked, this, &MainWindow::exportTimelineAnimation);
    viewControls->addWidget(timelineButton);
    radarOpacitySlider_ = new QSlider(Qt::Horizontal, root);
    radarOpacitySlider_->setRange(20, 100);
    radarOpacitySlider_->setValue(80);
    radarOpacitySlider_->setFixedWidth(90);
    radarOpacitySlider_->setToolTip(QStringLiteral("Radar opacity"));
    viewControls->addWidget(radarOpacitySlider_);
    connect(radarOpacitySlider_, &QSlider::valueChanged, this, [this](int opacity) {
        for (auto* panel : maps_) panel->setRadarOpacity(opacity);
    });
    belowThresholdCheck_ = new QCheckBox(QStringLiteral("Below threshold"), root);
    viewControls->addWidget(belowThresholdCheck_);
    connect(belowThresholdCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setBelowThresholdVisible(visible);
    });
    rootLayout->addLayout(viewControls);

    rootLayout->addWidget(mapHost, 1);

    auto* playback = new QHBoxLayout();
    playback->setSpacing(6);
    previousButton_ = new QToolButton(root);
    previousButton_->setObjectName(QStringLiteral("previousFrameButton"));
    previousButton_->setText(QStringLiteral("Previous"));
    playback->addWidget(previousButton_);
    playButton_ = new QToolButton(root);
    playButton_->setObjectName(QStringLiteral("playButton"));
    playButton_->setText(QStringLiteral("Play"));
    playButton_->setCheckable(true);
    playback->addWidget(playButton_);
    nextButton_ = new QToolButton(root);
    nextButton_->setObjectName(QStringLiteral("nextFrameButton"));
    nextButton_->setText(QStringLiteral("Next"));
    playback->addWidget(nextButton_);
    liveButton_ = new QToolButton(root);
    liveButton_->setObjectName(QStringLiteral("liveButton"));
    liveButton_->setText(QStringLiteral("LIVE"));
    liveButton_->setCheckable(true);
    liveButton_->setChecked(true);
    liveButton_->setToolTip(QStringLiteral("Follow the newest complete volume (L)"));
    playback->addWidget(liveButton_);
    frameSlider_ = new QSlider(Qt::Horizontal, root);
    frameSlider_->setObjectName(QStringLiteral("frameSlider"));
    frameSlider_->setRange(0, 0);
    frameSlider_->installEventFilter(this);
    playback->addWidget(frameSlider_, 1);
    frameLabel_ = new QLabel(QStringLiteral("No volume history"), root);
    frameLabel_->setMinimumWidth(185);
    playback->addWidget(frameLabel_);
    speedCombo_ = new QComboBox(root);
    speedCombo_->setObjectName(QStringLiteral("speedCombo"));
    speedCombo_->addItem(QStringLiteral("1x"), 1000);
    speedCombo_->addItem(QStringLiteral("2x"), 500);
    speedCombo_->addItem(QStringLiteral("4x"), 250);
    speedCombo_->setCurrentIndex(1);
    playback->addWidget(speedCombo_);
    overlayStatusLabel_ = new QLabel(root);
    overlayStatusLabel_->setObjectName(QStringLiteral("section"));
    overlayStatusLabel_->hide();
    rootLayout->addLayout(playback);

    auto* statusRow = new QHBoxLayout();
    unifiedStatusLabel_ = new QLabel(QStringLiteral("Waiting for live services"), root);
    unifiedStatusLabel_->setObjectName(QStringLiteral("section"));
    unifiedStatusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusRow->addWidget(unifiedStatusLabel_, 1);
    loadingProgress_ = new QProgressBar(root);
    loadingProgress_->setTextVisible(false);
    loadingProgress_->setFixedWidth(130);
    loadingProgress_->hide();
    statusRow->addWidget(loadingProgress_);
    retryButton_ = new QToolButton(root);
    retryButton_->setText(QStringLiteral("Retry"));
    retryButton_->hide();
    statusRow->addWidget(retryButton_);
    cancelButton_ = new QToolButton(root);
    cancelButton_->setText(QStringLiteral("Cancel"));
    cancelButton_->hide();
    statusRow->addWidget(cancelButton_);
    rootLayout->addLayout(statusRow);

    setCentralWidget(root);
    updateUnifiedStatus();
    updatePlaybackControls();
}

void MainWindow::rebuildStationPicker() {
    if (!stationCombo_) return;
    const QSignalBlocker blocker(stationCombo_);
    stationCombo_->clear();
    QSet<QString> added;
    const auto appendRadar = [this, &added](const QString& radarId) {
        if (radarId.isEmpty() || added.contains(radarId)) return;
        const auto row = model_->rowForRadar(radarId);
        if (row < 0) return;
        const auto& site = model_->siteAt(row);
        const auto label = QStringLiteral("%1%2 — %3 [%4]")
                               .arg(favoriteRadarIds_.contains(radarId) ? QStringLiteral("★ ") : QString{},
                                    radarId, QString::fromStdString(site.name),
                                    QString::fromStdString(site.owner));
        stationCombo_->addItem(label, radarId);
        added.insert(radarId);
    };
    auto favorites = favoriteRadarIds_.values();
    std::sort(favorites.begin(), favorites.end());
    for (const auto& radarId : favorites) appendRadar(radarId);
    for (const auto& radarId : recentRadarIds_) appendRadar(radarId);
    for (int row = 0; row < model_->rowCount(); ++row) {
        appendRadar(model_->index(row, 0).data(RadarListModel::RadarIdRole).toString());
    }
    if (auto* completer = stationCombo_->completer()) {
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setFilterMode(Qt::MatchContains);
        completer->setCompletionMode(QCompleter::PopupCompletion);
    }
    const auto current = stationCombo_->findData(selectedRadarId_);
    if (current >= 0) {
        stationCombo_->setCurrentIndex(current);
        stationCombo_->lineEdit()->setCursorPosition(0);
        stationCombo_->lineEdit()->deselect();
    }
}

void MainWindow::updateStationSummary() {
    const auto row = model_->rowForRadar(selectedRadarId_);
    if (row < 0) {
        stationSummaryLabel_->setText(QStringLiteral("Radar details unavailable"));
        return;
    }
    const auto& site = model_->siteAt(row);
    stationSummaryLabel_->setText(QStringLiteral("%1 · %2 · %3 m")
                                      .arg(QString::fromStdString(site.name),
                                           QString::fromStdString(site.owner))
                                      .arg(site.antennaElevationMeters, 0, 'f', 0));
    const QSignalBlocker blocker(favoriteButton_);
    const auto favorite = favoriteRadarIds_.contains(selectedRadarId_);
    favoriteButton_->setChecked(favorite);
    favoriteButton_->setText(favorite ? QStringLiteral("★") : QStringLiteral("☆"));
}

void MainWindow::updateUnifiedStatus() {
    if (!unifiedStatusLabel_) return;
    unifiedStatusLabel_->setText(QStringLiteral("Radar: %1    |    Overlays: %2")
                                     .arg(radarStatus_, overlayStatus_));
}

QString MainWindow::selectedRadarId() const {
    return selectedRadarId_;
}

std::shared_ptr<MainWindow::RadarSession> MainWindow::sessionFor(const QString& radarId, const bool create) {
    if (radarId.isEmpty()) return {};
    const auto existing = radarSessions_.find(radarId);
    if (existing != radarSessions_.end()) return existing.value();
    if (!create) return {};
    auto session = std::make_shared<RadarSession>();
    session->radarId = radarId;
    radarSessions_.insert(radarId, session);
    return session;
}

std::shared_ptr<MainWindow::RadarSession> MainWindow::activeSession() const {
    return radarSessions_.value(selectedRadarId_);
}

int MainWindow::panelIndex(const RadarMapCanvas* map) const {
    return maps_.indexOf(const_cast<RadarMapCanvas*>(map));
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    const auto session = activeSession();
    if (watched == frameSlider_ && event->type() == QEvent::MouseButtonPress &&
        session && session->history.size() < 2) {
        ensureHistoryLoaded(false);
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::radarSelected(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    const auto radarId = index.data(RadarListModel::RadarIdRole).toString();
    if (radarId == map_->radarId()) {
        map_->recenterSelected();
        return;
    }
    selectedRadarId_ = radarId;
    const auto activePanel = panelIndex(map_);
    while (panelRadarIds_.size() < maps_.size()) panelRadarIds_.push_back({});
    if (activePanel >= 0) panelRadarIds_[activePanel] = radarId;
    const auto existed = radarSessions_.contains(radarId);
    const auto session = sessionFor(radarId);
    recentRadarIds_.removeAll(radarId);
    recentRadarIds_.prepend(radarId);
    while (recentRadarIds_.size() > 8) recentRadarIds_.removeLast();
    QSettings settings;
    settings.setValue(QStringLiteral("recentRadars"), recentRadarIds_);
    settings.setValue(QStringLiteral("lastRadar"), radarId);
    if (activePanel >= 0) settings.setValue(QStringLiteral("panels/radar%1").arg(activePanel), radarId);
    rebuildStationPicker();
    stopAnimation();
    session->followLatest = true;
    liveButton_->setChecked(true);
    updateProductAvailability();
    updatePlaybackControls();
    map_->setSelectedRadar(radarId);
    map_->setLoading(session->history.empty(), QStringLiteral("Loading %1 radar data").arg(radarId));
    map_->setVolumeQuality(0, 0, 0);
    loadingProgress_->setRange(0, 0);
    loadingProgress_->show();
    cancelButton_->show();
    retryButton_->hide();
    tiltCombo_->clear();
    tiltCombo_->addItem(QStringLiteral("Waiting for decoded tilts"));
    tiltCombo_->setEnabled(false);
    radarStatus_ = QStringLiteral("%1 loading latest complete volume").arg(radarId);
    updateStationSummary();
    updateUnifiedStatus();
    if (!existed) source_->clearDownloaded(radarId);
    session->requestActive = true;
    source_->requestLatest(radarId);
    if (!session->history.empty()) {
        session->frameIndex = static_cast<int>(session->history.size()) - 1;
        showFrame(session->frameIndex);
    }
}

void MainWindow::requestSelectedRadar() {
    QSet<QString> requested;
    for (const auto& radarId : panelRadarIds_) {
        if (radarId.isEmpty() || requested.contains(radarId)) continue;
        requested.insert(radarId);
        if (const auto session = sessionFor(radarId)) session->requestActive = true;
        source_->requestLatest(radarId);
    }
}

void MainWindow::sourceStatusChanged(const QString& radarId, const QString& status) {
    model_->setStatus(radarId, status);
    const auto session = sessionFor(radarId, false);
    if (session && (status == QStringLiteral("no data") || status == QStringLiteral("no complete volume") ||
                    status == QStringLiteral("up to date") || status == QStringLiteral("history loaded") ||
                    status.startsWith(QStringLiteral("received ")))) session->requestActive = false;
    if (radarId == selectedRadarId()) {
        radarStatus_ = QStringLiteral("%1 %2").arg(radarId, status);
        statusLabel_->setText(radarStatus_);
        updateStationSummary();
        updateUnifiedStatus();
        if (status == QStringLiteral("no data") || status == QStringLiteral("no complete volume")) {
            for (auto* panel : maps_) if (panel->radarId() == radarId) panel->setLoading(false);
            loadingProgress_->hide();
            cancelButton_->hide();
        }
    }
}

void MainWindow::sourceFailed(const QString& radarId, const QString& message) {
    if (radarId == QStringLiteral("CATALOG")) {
        radarStatus_ = QStringLiteral("Live catalog unavailable (%1); using embedded Level II radar list")
                           .arg(message);
        updateUnifiedStatus();
        return;
    }
    model_->setStatus(radarId, QStringLiteral("error"));
    if (const auto session = sessionFor(radarId, false)) session->requestActive = false;
    if (radarId == selectedRadarId()) {
        radarStatus_ = QStringLiteral("%1 error: %2").arg(radarId, message);
        updateStationSummary();
        updateUnifiedStatus();
        for (auto* panel : maps_) if (panel->radarId() == radarId) panel->setLoading(false);
        loadingProgress_->hide();
        cancelButton_->hide();
        retryButton_->show();
    }
}

void MainWindow::catalogReceived(const QByteArray& data) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        radarStatus_ = QStringLiteral("Official radar catalog unavailable; using real offline catalog");
        updateUnifiedStatus();
        return;
    }

    std::vector<RadarSite> sites;
    const auto features = document.object().value(QStringLiteral("features")).toArray();
    for (const auto& value : features) {
        const auto feature = value.toObject();
        const auto properties = feature.value(QStringLiteral("properties")).toObject();
        const auto stationType = properties.value(QStringLiteral("stationType")).toString();
        if (stationType != QStringLiteral("WSR-88D") && stationType != QStringLiteral("TDWR")) {
            continue;
        }

        const auto id = properties.value(QStringLiteral("id")).toString();
        const auto geometry = feature.value(QStringLiteral("geometry")).toObject();
        const auto coordinates = geometry.value(QStringLiteral("coordinates")).toArray();
        const auto elevation = properties.value(QStringLiteral("elevation")).toObject().value(QStringLiteral("value"));
        if (id.size() != 4 || coordinates.size() < 2 || !elevation.isDouble()) {
            continue;
        }

        sites.push_back({
            id.toStdString(),
            properties.value(QStringLiteral("name")).toString().toStdString(),
            stationType.toStdString(),
            coordinates.at(1).toDouble(),
            coordinates.at(0).toDouble(),
            elevation.toDouble(),
        });
    }

    if (sites.empty()) {
        radarStatus_ = QStringLiteral("Official radar catalog empty; using real offline catalog");
        updateUnifiedStatus();
        return;
    }

    model_->replaceSites(std::move(sites));
    for (auto* panel : maps_) panel->setSites(model_->sites());
    rebuildStationPicker();
    radarStatus_ = QStringLiteral("Official NWS radar catalog loaded");
    updateUnifiedStatus();
    if (chaseMode_) {
        selectNearestChaseRadar();
        return;
    }
    auto row = model_->rowForRadar(selectedRadarId_);
    if (row < 0 && model_->rowCount() > 0) {
        row = 0;
    }
    if (row >= 0) {
        radarSelected(model_->index(row, 0));
    }
}

void MainWindow::volumeStarted(const QString& radarId, const QString& volumeId, const int chunkCount) {
    const auto session = sessionFor(radarId);
    session->pendingVolumeId = volumeId;
    session->pendingVolume = {};
    session->pendingVolume.radarId = radarId.toStdString();
    session->pendingChunkCount = chunkCount;
    session->pendingFailedChunks = 0;
    session->pendingDecodeTasks = 0;
    session->pendingVolumeNetworkReady = false;
    for (auto* panel : maps_) {
        if (panel->radarId() == radarId && session->history.empty())
            panel->setLoading(true, QStringLiteral("Downloading and decoding %1").arg(radarId));
    }
    if (radarId != selectedRadarId()) return;
    loadingProgress_->setRange(0, std::max(1, chunkCount));
    loadingProgress_->setValue(0);
    loadingProgress_->show();
    cancelButton_->show();
    radarStatus_ = QStringLiteral("%1 assembling %2 real volume chunks").arg(radarId).arg(chunkCount);
    updateUnifiedStatus();
}

void MainWindow::volumeProgress(const QString& radarId, const QString& volumeId, const int completed,
                                const int total, const int decoded, const int failed) {
    const auto session = sessionFor(radarId, false);
    if (!session || volumeId != session->pendingVolumeId) return;
    session->pendingFailedChunks = failed;
    for (auto* panel : maps_) {
        if (panel->radarId() == radarId)
            panel->setLoading(completed < total,
                              QStringLiteral("%1  %2/%3 chunks").arg(radarId).arg(completed).arg(total));
    }
    if (radarId != selectedRadarId()) return;
    loadingProgress_->setRange(0, std::max(1, total));
    loadingProgress_->setValue(completed);
    radarStatus_ = QStringLiteral("%1 volume chunks %2/%3 · %4 decoded%5")
                       .arg(radarId).arg(completed).arg(total).arg(decoded)
                       .arg(failed > 0 ? QStringLiteral(" · %1 unavailable").arg(failed) : QString{});
    updateUnifiedStatus();
}

void MainWindow::chunkDecompressed(const QString& radarId, const QString& volumeId, const QString&,
                                   const QByteArray& data, const QDateTime&) {
    const auto session = sessionFor(radarId, false);
    if (!session || volumeId != session->pendingVolumeId) return;
    ++session->pendingDecodeTasks;
    const QPointer<MainWindow> guard(this);
    QThreadPool::globalInstance()->start([guard, radarId, volumeId, data] {
        const auto* raw = reinterpret_cast<const std::byte*>(data.constData());
        auto decoded = decodeMessage31Chunk(
            std::span<const std::byte>(raw, static_cast<std::size_t>(data.size())), radarId.toStdString());
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, radarId, volumeId, decoded = std::move(decoded)]() mutable {
            if (guard) guard->acceptDecodedChunk(radarId, volumeId, std::move(decoded));
        }, Qt::QueuedConnection);
    });
}

void MainWindow::volumeReady(const QString& radarId, const QString& volumeId, const int chunkCount,
                             const int failedChunks, const QDateTime& receivedAt) {
    const auto session = sessionFor(radarId, false);
    if (!session || volumeId != session->pendingVolumeId) return;
    session->pendingVolumeNetworkReady = true;
    session->pendingReadyVolumeId = volumeId;
    session->pendingReadyChunkCount = chunkCount;
    session->pendingReadyFailedChunks = failedChunks;
    session->pendingReadyReceivedAt = receivedAt;
    if (session->pendingDecodeTasks > 0) {
        if (radarId == selectedRadarId()) {
            radarStatus_ = QStringLiteral("%1 decoding %2 radar chunk%3 in background")
                               .arg(radarId).arg(session->pendingDecodeTasks)
                               .arg(session->pendingDecodeTasks == 1 ? QString{} : QStringLiteral("s"));
            updateUnifiedStatus();
        }
        return;
    }
    session->pendingVolumeNetworkReady = false;
    model_->setStatus(radarId, QStringLiteral("received"), receivedAt);
    const auto followLatest = session->history.empty() || session->followLatest;
    const auto selectedVolume = session->frameIndex >= 0 &&
                                session->frameIndex < static_cast<int>(session->history.size())
                                    ? session->history.at(static_cast<std::size_t>(session->frameIndex)).volumeId
                                    : QString{};
    const auto duplicate = std::find_if(session->history.begin(), session->history.end(), [&volumeId](const RadarFrame& frame) {
        return frame.volumeId == volumeId;
    });
    if (duplicate == session->history.end()) {
        session->history.push_back({volumeId,
                                    std::make_shared<DecodedChunk>(std::move(session->pendingVolume)),
                                    receivedAt, chunkCount, failedChunks});
    }
    std::sort(session->history.begin(), session->history.end(), [](const RadarFrame& left, const RadarFrame& right) {
        return left.dataTime < right.dataTime;
    });
    const auto maximumHistory = static_cast<std::size_t>(std::max(2, maximumHistoryFrames_));
    if (session->history.size() > maximumHistory) {
        session->history.erase(session->history.begin(), session->history.begin() +
            static_cast<std::ptrdiff_t>(session->history.size() - maximumHistory));
    }
    session->pendingVolumeId.clear();
    session->requestActive = false;
    for (auto* panel : maps_) if (panel->radarId() == radarId) panel->setLoading(false);
    if (followLatest) {
        session->frameIndex = static_cast<int>(session->history.size()) - 1;
    } else {
        const auto selected = std::find_if(session->history.begin(), session->history.end(), [&selectedVolume](const RadarFrame& frame) {
            return frame.volumeId == selectedVolume;
        });
        session->frameIndex = selected == session->history.end() ? 0
            : static_cast<int>(std::distance(session->history.begin(), selected));
    }
    if (radarId == selectedRadarId()) {
        loadingProgress_->hide();
        cancelButton_->hide();
        retryButton_->hide();
        updatePlaybackControls();
        showFrame(session->frameIndex);
    } else if (!session->history.empty()) {
        const auto& frame = session->history.at(static_cast<std::size_t>(session->frameIndex));
        for (auto* panel : maps_) if (panel->radarId() == radarId) {
            panel->setDecodedChunk(frame.chunk, frame.dataTime);
            panel->setVolumeQuality(std::max(0, frame.chunkCount - frame.failedChunks),
                                    frame.chunkCount, frame.failedChunks,
                                    frame.chunk->rejectedMessages, frame.chunk->rejectedBlocks);
        }
    }
    radarStatus_ = QStringLiteral("%1 · %2 real volume%3 · %4")
                       .arg(radarId).arg(session->history.size())
                       .arg(session->history.size() == 1 ? QString{} : QStringLiteral("s"))
                       .arg(failedChunks == 0 ? QStringLiteral("complete")
                                              : QStringLiteral("partial: %1 chunk%2 unavailable")
                                                    .arg(failedChunks)
                                                    .arg(failedChunks == 1 ? QString{} : QStringLiteral("s")));
    if (radarId == selectedRadarId()) {
        updateStationSummary();
        updateUnifiedStatus();
    }
    if (session->historyRequested && !session->historyLoadStarted) {
        QTimer::singleShot(0, this, [this, radarId] {
            const auto current = sessionFor(radarId, false);
            if (!current || !current->historyRequested || current->historyLoadStarted) return;
            current->historyLoadStarted = true;
            current->requestActive = true;
            if (radarId == selectedRadarId_) {
                radarStatus_ = QStringLiteral("%1 loading timeline history").arg(radarId);
                updateUnifiedStatus();
            }
            source_->requestRecent(radarId, historyVolumeCount_);
        });
    }
    if (radarId == selectedRadarId() && session->playWhenHistoryReady && session->history.size() >= 2) {
        session->playWhenHistoryReady = false;
        QTimer::singleShot(0, this, [this] { playButton_->setChecked(true); });
    }
}

void MainWindow::acceptDecodedChunk(const QString& radarId, const QString& volumeId, DecodedChunk decoded) {
    const auto session = sessionFor(radarId, false);
    if (!session || volumeId != session->pendingVolumeId) return;
    session->pendingVolume.messageCount += decoded.messageCount;
    session->pendingVolume.rejectedMessages += decoded.rejectedMessages;
    session->pendingVolume.rejectedBlocks += decoded.rejectedBlocks;
    session->pendingVolume.rays.insert(session->pendingVolume.rays.end(),
                                       std::make_move_iterator(decoded.rays.begin()),
                                       std::make_move_iterator(decoded.rays.end()));
    session->pendingDecodeTasks = std::max(0, session->pendingDecodeTasks - 1);
    if (session->pendingVolumeNetworkReady && session->pendingDecodeTasks == 0) {
        volumeReady(radarId, session->pendingReadyVolumeId, session->pendingReadyChunkCount,
                    session->pendingReadyFailedChunks, session->pendingReadyReceivedAt);
    }
}

void MainWindow::ensureHistoryLoaded(const bool playWhenReady) {
    const auto session = activeSession();
    if (!session) return;
    if (playWhenReady) session->playWhenHistoryReady = true;
    if (session->history.size() >= 2) {
        if (playWhenReady && !playButton_->isChecked()) playButton_->setChecked(true);
        return;
    }
    session->historyRequested = true;
    radarStatus_ = QStringLiteral("%1 timeline history requested").arg(selectedRadarId_);
    updateUnifiedStatus();
    if (session->requestActive || session->historyLoadStarted || selectedRadarId_.isEmpty()) return;
    session->historyLoadStarted = true;
    session->requestActive = true;
    source_->requestRecent(selectedRadarId_, historyVolumeCount_);
}

void MainWindow::rebuildTiltCombo() {
    struct TiltSummary {
        int number = 0;
        double angleTotal = 0.0;
        int rayCount = 0;
    };
    const auto previousElevation = map_->elevationFilter().value_or(-1);
    std::vector<TiltSummary> tilts;
    const auto session = activeSession();
    if (!session || session->frameIndex < 0 ||
        session->frameIndex >= static_cast<int>(session->history.size())) {
        tiltCombo_->clear();
        tiltCombo_->addItem(QStringLiteral("Waiting for decoded tilts"));
        tiltCombo_->setEnabled(false);
        map_->setElevationFilter(std::nullopt);
        return;
    }
    const auto& currentVolume = *session->history.at(static_cast<std::size_t>(session->frameIndex)).chunk;
    const auto product = map_->product();
    for (const auto& ray : currentVolume.rays) {
        if (!ray.has(product)) continue;
        auto existing = std::find_if(tilts.begin(), tilts.end(), [&ray](const TiltSummary& tilt) {
            return tilt.number == ray.elevationNumber;
        });
        if (existing == tilts.end()) {
            tilts.push_back({ray.elevationNumber, ray.elevationDegrees, 1});
        } else {
            existing->angleTotal += ray.elevationDegrees;
            ++existing->rayCount;
        }
    }
    std::sort(tilts.begin(), tilts.end(), [](const TiltSummary& left, const TiltSummary& right) {
        return left.number < right.number;
    });
    QHash<int, int> scansPerAngle;
    for (const auto& tilt : tilts) {
        const auto angleTenths = static_cast<int>(std::round(
            tilt.angleTotal / std::max(tilt.rayCount, 1) * 10.0));
        ++scansPerAngle[angleTenths];
    }
    QHash<int, int> scanIndex;
    const QSignalBlocker blocker(tiltCombo_);
    tiltCombo_->clear();
    int selectedIndex = -1;
    for (const auto& tilt : tilts) {
        const auto meanAngle = tilt.angleTotal / std::max(tilt.rayCount, 1);
        const auto angleTenths = static_cast<int>(std::round(meanAngle * 10.0));
        const auto scan = ++scanIndex[angleTenths];
        const auto suffix = scansPerAngle.value(angleTenths) > 1
                                ? QStringLiteral(" · scan %1/%2")
                                      .arg(scan).arg(scansPerAngle.value(angleTenths))
                                : QString{};
        tiltCombo_->addItem(QStringLiteral("%1° · tilt %2%3")
                                .arg(meanAngle, 0, 'f', 1).arg(tilt.number).arg(suffix),
                            tilt.number);
        if (tilt.number == previousElevation) selectedIndex = tiltCombo_->count() - 1;
    }
    tiltCombo_->setEnabled(!tilts.empty());
    if (!tilts.empty()) {
        if (selectedIndex < 0) selectedIndex = 0;
        tiltCombo_->setCurrentIndex(selectedIndex);
        map_->setElevationFilter(tiltCombo_->itemData(selectedIndex).toInt());
    } else {
        map_->setElevationFilter(std::nullopt);
    }
}

void MainWindow::showFrame(const int index) {
    const auto session = activeSession();
    if (!session || index < 0 || index >= static_cast<int>(session->history.size())) return;
    session->frameIndex = index;
    const QSignalBlocker blocker(frameSlider_);
    frameSlider_->setValue(session->frameIndex);
    const auto& frame = session->history.at(static_cast<std::size_t>(session->frameIndex));
    for (auto* panel : maps_) {
        const auto panelSession = sessionFor(panel->radarId(), false);
        if (!panelSession || panelSession->history.empty()) continue;
        auto selected = panelSession->frameIndex >= 0 ? panelSession->frameIndex
                                                       : static_cast<int>(panelSession->history.size()) - 1;
        if (panel != map_) {
            qint64 bestDifference = std::numeric_limits<qint64>::max();
            for (int candidate = 0; candidate < static_cast<int>(panelSession->history.size()); ++candidate) {
                const auto difference = std::abs(panelSession->history.at(static_cast<std::size_t>(candidate))
                                                     .dataTime.secsTo(frame.dataTime));
                if (difference < bestDifference) {
                    bestDifference = difference;
                    selected = candidate;
                }
            }
        }
        panelSession->frameIndex = selected;
        const auto& panelFrame = panelSession->history.at(static_cast<std::size_t>(selected));
        panel->setDecodedChunk(panelFrame.chunk, panelFrame.dataTime);
        panel->setVolumeQuality(std::max(0, panelFrame.chunkCount - panelFrame.failedChunks),
                                panelFrame.chunkCount, panelFrame.failedChunks,
                                panelFrame.chunk->rejectedMessages, panelFrame.chunk->rejectedBlocks);
    }
    updateProductAvailability();
    rebuildTiltCombo();
    const auto ageSeconds = std::max<qint64>(0, frame.dataTime.secsTo(QDateTime::currentDateTimeUtc()));
    const auto ageText = ageSeconds < 60 ? QStringLiteral("now")
                         : ageSeconds < 3600 ? QStringLiteral("%1m ago").arg(ageSeconds / 60)
                                             : QStringLiteral("%1h %2m ago")
                                                   .arg(ageSeconds / 3600)
                                                   .arg((ageSeconds % 3600) / 60);
    const auto isLatest = session->frameIndex == static_cast<int>(session->history.size()) - 1;
    frameLabel_->setText(QStringLiteral("%1/%2 · %3 · %4%5")
                             .arg(session->frameIndex + 1).arg(session->history.size())
                             .arg(frame.dataTime.toLocalTime().toString(QStringLiteral("HH:mm:ss")))
                             .arg(ageText)
                             .arg(isLatest && session->followLatest ? QStringLiteral(" · LIVE") : QString{}));
    frameSlider_->setToolTip(QStringLiteral("Frame %1 of %2\n%3\n%4")
                                 .arg(session->frameIndex + 1).arg(session->history.size())
                                 .arg(frame.dataTime.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss t")))
                                 .arg(frame.failedChunks == 0 ? QStringLiteral("Complete volume")
                                                              : QStringLiteral("Partial: %1 chunks unavailable")
                                                                    .arg(frame.failedChunks)));
    {
        const QSignalBlocker liveBlocker(liveButton_);
        liveButton_->setChecked(session->followLatest && isLatest);
    }
    updatePlaybackControls();
}

void MainWindow::updateProductAvailability() {
    auto* itemModel = qobject_cast<QStandardItemModel*>(productCombo_->model());
    if (!itemModel) return;
    const auto session = activeSession();
    const auto hasFrame = session && session->frameIndex >= 0 &&
                          session->frameIndex < static_cast<int>(session->history.size());
    int firstAvailable = -1;
    for (int row = 0; row < productCombo_->count(); ++row) {
        const auto product = static_cast<ProductKind>(productCombo_->itemData(row).toInt());
        const auto available = !hasFrame || std::any_of(
            session->history.at(static_cast<std::size_t>(session->frameIndex)).chunk->rays.cbegin(),
            session->history.at(static_cast<std::size_t>(session->frameIndex)).chunk->rays.cend(),
            [product](const DecodedRay& ray) { return ray.has(product); });
        if (auto* item = itemModel->item(row)) item->setEnabled(available);
        if (available && firstAvailable < 0) firstAvailable = row;
    }
    if (hasFrame && firstAvailable >= 0) {
        const auto currentItem = itemModel->item(productCombo_->currentIndex());
        if (!currentItem || !currentItem->isEnabled()) productCombo_->setCurrentIndex(firstAvailable);
    }
}

void MainWindow::advanceFrame() {
    const auto session = activeSession();
    if (!session || session->history.size() < 2) {
        stopAnimation();
        return;
    }
    session->followLatest = false;
    showFrame((session->frameIndex + 1) % static_cast<int>(session->history.size()));
}

void MainWindow::updatePlaybackControls() {
    const auto session = activeSession();
    const auto hasFrames = session && !session->history.empty();
    const auto canAnimate = session && session->history.size() > 1;
    frameSlider_->setRange(0, session ? std::max(0, static_cast<int>(session->history.size()) - 1) : 0);
    previousButton_->setEnabled(hasFrames && session->frameIndex > 0);
    nextButton_->setEnabled(hasFrames && session->frameIndex + 1 < static_cast<int>(session->history.size()));
    playButton_->setEnabled(hasFrames);
    frameSlider_->setEnabled(hasFrames);
    speedCombo_->setEnabled(canAnimate);
    liveButton_->setEnabled(hasFrames);
    if (!hasFrames) frameLabel_->setText(QStringLiteral("Waiting for latest volume"));
    else if (!canAnimate && session && !session->historyRequested) {
        frameLabel_->setText(QStringLiteral("LIVE · click timeline or Play to load history"));
        frameSlider_->setToolTip(QStringLiteral("Click to load historical volumes"));
    }
}

void MainWindow::stopAnimation() {
    animationTimer_->stop();
    if (playButton_->isChecked()) playButton_->setChecked(false);
    playButton_->setText(QStringLiteral("Play"));
}

void MainWindow::restoreSettings() {
    QSettings settings;
    maximumHistoryFrames_ = std::clamp(settings.value(QStringLiteral("timeline/maximumFrames"), 12).toInt(),
                                       2, 24);
    historyVolumeCount_ = std::clamp(settings.value(QStringLiteral("timeline/historyVolumes"), 8).toInt(),
                                    2, 12);
    chaseMode_ = settings.value(QStringLiteral("chase/enabled"), false).toBool();
    chaseLatitude_ = std::clamp(settings.value(QStringLiteral("chase/latitude"), 35.0).toDouble(), -90.0, 90.0);
    chaseLongitude_ = std::clamp(settings.value(QStringLiteral("chase/longitude"), -97.0).toDouble(), -180.0, 180.0);
    synchronizePanels_ = settings.value(QStringLiteral("panels/synchronizeNavigation"), true).toBool();
    for (int panel = 0; panel < maps_.size(); ++panel) {
        const auto defaultProduct = static_cast<int>(maps_.at(panel)->product());
        const auto product = std::clamp(settings.value(QStringLiteral("panels/product%1").arg(panel),
                                                        defaultProduct).toInt(), 0, 5);
        maps_.at(panel)->setProduct(static_cast<ProductKind>(product));
        const auto savedTilt = settings.value(QStringLiteral("panels/tilt%1").arg(panel), -1).toInt();
        maps_.at(panel)->setElevationFilter(savedTilt > 0 ? std::optional<int>(savedTilt) : std::nullopt);
        maps_.at(panel)->setVelocityProcessing({
            static_cast<VelocityProcessingMode>(std::clamp(
                settings.value(QStringLiteral("panels/velocityMode%1").arg(panel), 0).toInt(), 0, 2)),
            std::clamp(settings.value(QStringLiteral("panels/nyquist%1").arg(panel), 25.0).toDouble(), 1.0, 80.0),
            std::clamp(settings.value(QStringLiteral("panels/stormDirection%1").arg(panel), 0.0).toDouble(), 0.0, 359.9),
            std::clamp(settings.value(QStringLiteral("panels/stormSpeed%1").arg(panel), 0.0).toDouble(), 0.0, 80.0),
        });
        panelRadarIds_[panel] = settings.value(QStringLiteral("panels/radar%1").arg(panel)).toString();
    }

    QVector<RadarMapCanvas::PaletteStop> customStops;
    for (const auto& encoded : settings.value(QStringLiteral("view/customPalette")).toStringList()) {
        const auto fields = encoded.split(QLatin1Char(','));
        bool ok = false;
        const auto value = fields.value(0).toDouble(&ok);
        const QColor color(fields.value(1));
        if (ok && color.isValid()) customStops.push_back({value, color});
    }
    for (auto* panel : maps_) panel->setCustomPalette(customStops);
    productCombo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("view/product"), 0).toInt(),
                                              0, productCombo_->count() - 1));
    paletteCombo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("view/palette"), 1).toInt(),
                                              0, paletteCombo_->count() - 1));
    radarOpacitySlider_->setValue(std::clamp(settings.value(QStringLiteral("view/radarOpacity"), 80).toInt(),
                                             radarOpacitySlider_->minimum(), radarOpacitySlider_->maximum()));
    warningFilterCombo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("overlays/warningFilter"), 0).toInt(),
                                                    0, warningFilterCombo_->count() - 1));
    warningsCheck_->setChecked(settings.value(QStringLiteral("overlays/warnings"), true).toBool());
    reportsCheck_->setChecked(settings.value(QStringLiteral("overlays/reports"), true).toBool());
    tornadoCheck_->setChecked(settings.value(QStringLiteral("overlays/tornado"), true).toBool());
    hailCheck_->setChecked(settings.value(QStringLiteral("overlays/hail"), true).toBool());
    windCheck_->setChecked(settings.value(QStringLiteral("overlays/wind"), true).toBool());
    belowThresholdCheck_->setChecked(settings.value(QStringLiteral("view/belowThreshold"), false).toBool());
    speedCombo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("timeline/speed"), 1).toInt(),
                                            0, speedCombo_->count() - 1));
    panelLayoutCombo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("panels/layout"), 0).toInt(),
                                                  0, panelLayoutCombo_->count() - 1));
    activateMap(maps_.front());
}

void MainWindow::saveSettings() const {
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("view/product"), productCombo_->currentIndex());
    settings.setValue(QStringLiteral("view/palette"), paletteCombo_->currentIndex());
    QStringList customStops;
    for (const auto& stop : map_->customPalette())
        customStops.push_back(QStringLiteral("%1,%2").arg(stop.value, 0, 'g', 12).arg(stop.color.name(QColor::HexArgb)));
    settings.setValue(QStringLiteral("view/customPalette"), customStops);
    settings.setValue(QStringLiteral("view/radarOpacity"), radarOpacitySlider_->value());
    settings.setValue(QStringLiteral("view/belowThreshold"), belowThresholdCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/warnings"), warningsCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/reports"), reportsCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/tornado"), tornadoCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/hail"), hailCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/wind"), windCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/warningFilter"), warningFilterCombo_->currentIndex());
    settings.setValue(QStringLiteral("timeline/speed"), speedCombo_->currentIndex());
    settings.setValue(QStringLiteral("timeline/maximumFrames"), maximumHistoryFrames_);
    settings.setValue(QStringLiteral("timeline/historyVolumes"), historyVolumeCount_);
    settings.setValue(QStringLiteral("chase/enabled"), chaseMode_);
    settings.setValue(QStringLiteral("chase/latitude"), chaseLatitude_);
    settings.setValue(QStringLiteral("chase/longitude"), chaseLongitude_);
    settings.setValue(QStringLiteral("panels/layout"), panelLayoutCombo_->currentIndex());
    settings.setValue(QStringLiteral("panels/synchronizeNavigation"), synchronizePanels_);
    for (int panel = 0; panel < maps_.size(); ++panel) {
        settings.setValue(QStringLiteral("panels/product%1").arg(panel),
                          static_cast<int>(maps_.at(panel)->product()));
        settings.setValue(QStringLiteral("panels/tilt%1").arg(panel),
                          maps_.at(panel)->elevationFilter().value_or(-1));
        const auto velocity = maps_.at(panel)->velocityProcessing();
        settings.setValue(QStringLiteral("panels/velocityMode%1").arg(panel), static_cast<int>(velocity.mode));
        settings.setValue(QStringLiteral("panels/nyquist%1").arg(panel), velocity.nyquistVelocityMetersPerSecond);
        settings.setValue(QStringLiteral("panels/stormDirection%1").arg(panel), velocity.stormMotionDirectionDegrees);
        settings.setValue(QStringLiteral("panels/stormSpeed%1").arg(panel), velocity.stormMotionSpeedMetersPerSecond);
        settings.setValue(QStringLiteral("panels/radar%1").arg(panel), maps_.at(panel)->radarId());
    }
}

void MainWindow::showSettingsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("CappedWeather Settings"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* chaseCheck = new QCheckBox(QStringLiteral("Select the nearest radar to the chase position"), &dialog);
    chaseCheck->setChecked(chaseMode_);
    auto* latitude = new QDoubleSpinBox(&dialog);
    latitude->setRange(-90.0, 90.0);
    latitude->setDecimals(5);
    latitude->setValue(chaseLatitude_);
    auto* longitude = new QDoubleSpinBox(&dialog);
    longitude->setRange(-180.0, 180.0);
    longitude->setDecimals(5);
    longitude->setValue(chaseLongitude_);
    auto* frames = new QSpinBox(&dialog);
    frames->setRange(2, 24);
    frames->setValue(maximumHistoryFrames_);
    auto* volumes = new QSpinBox(&dialog);
    volumes->setRange(2, 12);
    volumes->setValue(historyVolumeCount_);
    auto* contact = new QLineEdit(QSettings().value(QStringLiteral("network/contact")).toString(), &dialog);
    contact->setPlaceholderText(QStringLiteral("Email or project URL for public-service requests"));
    auto* synchronize = new QCheckBox(QStringLiteral("Link pan and zoom across radar panels"), &dialog);
    synchronize->setChecked(synchronizePanels_);
    auto* parallelChunks = new QSpinBox(&dialog);
    parallelChunks->setRange(1, 12);
    parallelChunks->setValue(QSettings().value(QStringLiteral("network/parallelChunks"), 6).toInt());
    auto* timeout = new QSpinBox(&dialog);
    timeout->setRange(5, 120);
    timeout->setSuffix(QStringLiteral(" s"));
    timeout->setValue(QSettings().value(QStringLiteral("network/timeoutSeconds"), 30).toInt());
    auto* retries = new QSpinBox(&dialog);
    retries->setRange(0, 5);
    retries->setValue(QSettings().value(QStringLiteral("network/retries"), 2).toInt());
    auto* backoff = new QSpinBox(&dialog);
    backoff->setRange(100, 5000);
    backoff->setSingleStep(100);
    backoff->setSuffix(QStringLiteral(" ms"));
    backoff->setValue(QSettings().value(QStringLiteral("network/retryBackoffMs"), 250).toInt());
    auto* radarRefresh = new QSpinBox(&dialog);
    radarRefresh->setRange(30, 600);
    radarRefresh->setSuffix(QStringLiteral(" s"));
    radarRefresh->setValue(QSettings().value(QStringLiteral("network/radarRefreshSeconds"), 60).toInt());
    auto* overlayRefresh = new QSpinBox(&dialog);
    overlayRefresh->setRange(30, 600);
    overlayRefresh->setSuffix(QStringLiteral(" s"));
    overlayRefresh->setValue(QSettings().value(QStringLiteral("network/overlayRefreshSeconds"), 60).toInt());
    auto* memoryCache = new QSpinBox(&dialog);
    memoryCache->setRange(8, 256);
    memoryCache->setSuffix(QStringLiteral(" MB/panel"));
    memoryCache->setValue(QSettings().value(QStringLiteral("cache/memoryMbPerPanel"), 32).toInt());
    auto* diskCache = new QSpinBox(&dialog);
    diskCache->setRange(16, 1024);
    diskCache->setSuffix(QStringLiteral(" MB/panel"));
    diskCache->setValue(QSettings().value(QStringLiteral("cache/diskMbPerPanel"), 64).toInt());
    form->addRow(QStringLiteral("Chase mode"), chaseCheck);
    form->addRow(QStringLiteral("Chase latitude"), latitude);
    form->addRow(QStringLiteral("Chase longitude"), longitude);
    form->addRow(QStringLiteral("Frames kept in memory"), frames);
    form->addRow(QStringLiteral("Volumes loaded for playback"), volumes);
    form->addRow(QStringLiteral("Network contact"), contact);
    form->addRow(QStringLiteral("Parallel radar chunks"), parallelChunks);
    form->addRow(QStringLiteral("Request timeout"), timeout);
    form->addRow(QStringLiteral("Chunk retries"), retries);
    form->addRow(QStringLiteral("Initial retry backoff"), backoff);
    form->addRow(QStringLiteral("Radar refresh"), radarRefresh);
    form->addRow(QStringLiteral("Warning/report refresh"), overlayRefresh);
    form->addRow(QStringLiteral("Map memory cache (restart)"), memoryCache);
    form->addRow(QStringLiteral("Map disk cache (restart)"), diskCache);
    form->addRow(QStringLiteral("Multi-panel"), synchronize);
    layout->addLayout(form);
    auto* note = new QLabel(QStringLiteral(
        "Chase mode uses the configured position and automatically selects the nearest operational radar. "
        "The position can be updated whenever the chase moves."), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    chaseMode_ = chaseCheck->isChecked();
    chaseLatitude_ = latitude->value();
    chaseLongitude_ = longitude->value();
    maximumHistoryFrames_ = frames->value();
    historyVolumeCount_ = volumes->value();
    synchronizePanels_ = synchronize->isChecked();
    QSettings networkSettings;
    networkSettings.setValue(QStringLiteral("network/contact"), contact->text().trimmed());
    networkSettings.setValue(QStringLiteral("network/parallelChunks"), parallelChunks->value());
    networkSettings.setValue(QStringLiteral("network/timeoutSeconds"), timeout->value());
    networkSettings.setValue(QStringLiteral("network/retries"), retries->value());
    networkSettings.setValue(QStringLiteral("network/retryBackoffMs"), backoff->value());
    networkSettings.setValue(QStringLiteral("network/radarRefreshSeconds"), radarRefresh->value());
    networkSettings.setValue(QStringLiteral("network/overlayRefreshSeconds"), overlayRefresh->value());
    networkSettings.setValue(QStringLiteral("cache/memoryMbPerPanel"), memoryCache->value());
    networkSettings.setValue(QStringLiteral("cache/diskMbPerPanel"), diskCache->value());
    refreshTimer_->setInterval(radarRefresh->value() * 1000);
    overlayRefreshTimer_->setInterval(overlayRefresh->value() * 1000);
    for (const auto& session : radarSessions_) {
        while (session->history.size() > static_cast<std::size_t>(maximumHistoryFrames_))
            session->history.erase(session->history.begin());
    }
    saveSettings();
    if (chaseMode_) selectNearestChaseRadar();
}

void MainWindow::selectNearestChaseRadar() {
    if (!chaseMode_ || model_->rowCount() == 0) return;
    auto bestRow = -1;
    auto bestDistance = std::numeric_limits<double>::max();
    const auto latitudeRadians = qDegreesToRadians(chaseLatitude_);
    for (int row = 0; row < model_->rowCount(); ++row) {
        const auto& site = model_->siteAt(row);
        if (!site.latitude || !site.longitude) continue;
        const auto dLatitude = qDegreesToRadians(*site.latitude - chaseLatitude_);
        const auto dLongitude = qDegreesToRadians(*site.longitude - chaseLongitude_);
        const auto siteLatitude = qDegreesToRadians(*site.latitude);
        const auto a = std::sin(dLatitude / 2.0) * std::sin(dLatitude / 2.0) +
                       std::cos(latitudeRadians) * std::cos(siteLatitude) *
                           std::sin(dLongitude / 2.0) * std::sin(dLongitude / 2.0);
        const auto angularDistance = 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
        if (angularDistance < bestDistance) {
            bestDistance = angularDistance;
            bestRow = row;
        }
    }
    if (bestRow >= 0) radarSelected(model_->index(bestRow, 0));
}

void MainWindow::setPanelLayout(const int layoutIndex) {
    if (!mapGrid_ || maps_.size() < 4) return;
    for (auto* panel : maps_) {
        mapGrid_->removeWidget(panel);
        panel->hide();
    }
    const auto add = [this](const int panel, const int row, const int column,
                            const int rowSpan = 1, const int columnSpan = 1) {
        mapGrid_->addWidget(maps_.at(panel), row, column, rowSpan, columnSpan);
        maps_.at(panel)->show();
    };
    if (layoutIndex == 1) {
        add(0, 0, 0);
        add(1, 0, 1);
    } else if (layoutIndex == 2) {
        add(0, 0, 0);
        add(1, 1, 0);
    } else if (layoutIndex == 3) {
        add(0, 0, 0);
        add(1, 0, 1);
        add(2, 1, 0);
        add(3, 1, 1);
    } else {
        add(0, 0, 0, 2, 2);
    }
    mapGrid_->setRowStretch(0, 1);
    mapGrid_->setRowStretch(1, 1);
    mapGrid_->setColumnStretch(0, 1);
    mapGrid_->setColumnStretch(1, 1);
    if (panelLayoutCombo_ && map_->isHidden()) activateMap(maps_.front());
    if (panelLayoutCombo_) saveSettings();
}

void MainWindow::activateMap(RadarMapCanvas* map) {
    if (!map || map_ == map && map->property("activePanel").toBool()) return;
    map_ = map;
    for (auto* panel : maps_) {
        panel->setActivePanel(panel == map_);
        panel->setProperty("activePanel", panel == map_);
        if (panel != map_) {
            panel->setMeasureMode(false);
            panel->setCrossSectionMode(false);
        }
    }
    {
        const QSignalBlocker blocker(productCombo_);
        const auto index = productCombo_->findData(static_cast<int>(map_->product()));
        if (index >= 0) productCombo_->setCurrentIndex(index);
    }
    const auto velocity = map_->velocityProcessing();
    {
        const QSignalBlocker modeBlocker(velocityModeCombo_);
        const QSignalBlocker nyquistBlocker(nyquistSpin_);
        const QSignalBlocker directionBlocker(stormDirectionSpin_);
        const QSignalBlocker speedBlocker(stormSpeedSpin_);
        velocityModeCombo_->setCurrentIndex(static_cast<int>(velocity.mode));
        nyquistSpin_->setValue(velocity.nyquistVelocityMetersPerSecond);
        stormDirectionSpin_->setValue(velocity.stormMotionDirectionDegrees);
        stormSpeedSpin_->setValue(velocity.stormMotionSpeedMetersPerSecond);
    }
    const QSignalBlocker measureBlocker(measureButton_);
    const QSignalBlocker crossSectionBlocker(crossSectionButton_);
    measureButton_->setChecked(false);
    crossSectionButton_->setChecked(false);
    selectedRadarId_ = map_->radarId();
    rebuildStationPicker();
    updateStationSummary();
    updateProductAvailability();
    rebuildTiltCombo();
    updateVelocityControls();
    updatePlaybackControls();
    const auto session = activeSession();
    if (session && !session->history.empty()) {
        if (session->frameIndex < 0) session->frameIndex = static_cast<int>(session->history.size()) - 1;
        showFrame(session->frameIndex);
    }
}

void MainWindow::synchronizeMapView(RadarMapCanvas* source) {
    if (!synchronizePanels_ || synchronizingMapView_ || !source) return;
    synchronizingMapView_ = true;
    for (auto* panel : maps_) {
        if (panel != source) panel->setView(source->viewCenter(), source->viewZoom());
    }
    synchronizingMapView_ = false;
}

void MainWindow::updateVelocityControls() {
    if (!velocityModeCombo_ || !map_) return;
    const auto velocityProduct = map_->product() == ProductKind::Velocity;
    velocityModeCombo_->setEnabled(velocityProduct);
    const auto derived = velocityProduct && velocityModeCombo_->currentIndex() != 0;
    nyquistSpin_->setEnabled(derived);
    const auto stormRelative = velocityProduct && velocityModeCombo_->currentIndex() == 2;
    stormDirectionSpin_->setEnabled(stormRelative);
    stormSpeedSpin_->setEnabled(stormRelative);
}

void MainWindow::showCrossSection(const QPointF& start, const QPointF& end) {
    const auto session = activeSession();
    if (!session || session->frameIndex < 0 ||
        session->frameIndex >= static_cast<int>(session->history.size())) return;
    const auto row = model_->rowForRadar(selectedRadarId_);
    if (row < 0) return;
    const auto& site = model_->siteAt(row);
    if (!site.latitude || !site.longitude) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("%1 Cross-section").arg(selectedRadarId_));
    auto* layout = new QVBoxLayout(&dialog);
    const auto& frame = session->history.at(static_cast<std::size_t>(session->frameIndex));
    auto* crossSection = new CrossSectionWidget(
        frame.chunk, {*site.latitude, *site.longitude}, site.antennaElevationMeters,
        start, end, map_->product(), map_->palette(), map_->customPalette(),
        map_->velocityProcessing(), &dialog);
    layout->addWidget(crossSection);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* exportButton = buttons->addButton(QStringLiteral("Export CSV"), QDialogButtonBox::ActionRole);
    connect(exportButton, &QPushButton::clicked, &dialog, [this, crossSection] {
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Export cross-section"),
                                                       QStringLiteral("cross-section.csv"),
                                                       QStringLiteral("CSV files (*.csv)"));
        if (!path.isEmpty() && !crossSection->exportCsv(path))
            QMessageBox::warning(this, QStringLiteral("Export failed"), QStringLiteral("Could not write %1").arg(path));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::importCustomPalette() {
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Import radar palette"), {},
                                                   QStringLiteral("Palette files (*.txt *.pal *.csv);;All files (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Palette error"), QStringLiteral("Could not open %1").arg(path));
        return;
    }
    QVector<RadarMapCanvas::PaletteStop> stops;
    QTextStream stream(&file);
    int lineNumber = 0;
    while (!stream.atEnd()) {
        ++lineNumber;
        const auto line = stream.readLine().trimmed();
        if (line.isEmpty() || (line.startsWith(QLatin1Char('#')) && !line.contains(QLatin1Char(',')))) continue;
        const auto fields = line.split(QLatin1Char(','), Qt::SkipEmptyParts);
        bool valueOk = false;
        const auto value = fields.value(0).trimmed().toDouble(&valueOk);
        const QColor color(fields.value(1).trimmed());
        if (fields.size() < 2 || !valueOk || !color.isValid()) {
            QMessageBox::warning(this, QStringLiteral("Palette error"),
                                 QStringLiteral("Invalid value,color entry on line %1").arg(lineNumber));
            return;
        }
        stops.push_back({value, color});
    }
    if (stops.size() < 2) {
        QMessageBox::warning(this, QStringLiteral("Palette error"), QStringLiteral("A palette needs at least two stops."));
        return;
    }
    for (auto* panel : maps_) {
        panel->setCustomPalette(stops);
        panel->setPalette(RadarMapCanvas::PaletteKind::Custom);
    }
    paletteCombo_->setCurrentIndex(static_cast<int>(RadarMapCanvas::PaletteKind::Custom));
    statusBar()->showMessage(QStringLiteral("Imported %1 palette stops from %2").arg(stops.size()).arg(path), 5000);
}

void MainWindow::exportSnapshot() {
    const auto defaultName = QStringLiteral("%1-%2-%3.png")
                                 .arg(selectedRadarId_, QString::fromUtf8(productDescriptor(map_->product()).shortName.data()),
                                      QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Export map snapshot"), defaultName,
                                                   QStringLiteral("PNG image (*.png)"));
    if (path.isEmpty()) return;
    if (!map_->grab().save(path, "PNG"))
        QMessageBox::warning(this, QStringLiteral("Export failed"), QStringLiteral("Could not write %1").arg(path));
}

void MainWindow::exportTimelineAnimation() {
    const auto session = activeSession();
    if (!session || session->history.empty()) return;
    const auto defaultName = QStringLiteral("%1-%2-timeline.gif")
                                 .arg(selectedRadarId_, QString::fromUtf8(productDescriptor(map_->product()).shortName.data()));
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Export timeline animation"),
                                                   defaultName, QStringLiteral("Animated GIF (*.gif)"));
    if (path.isEmpty()) return;
    const auto previousIndex = session->frameIndex;
    QVector<QImage> frames;
    frames.reserve(static_cast<qsizetype>(session->history.size()));
    for (int index = 0; index < static_cast<int>(session->history.size()); ++index) {
        showFrame(index);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        frames.push_back(map_->grab().toImage());
    }
    showFrame(previousIndex);
    QString error;
    const auto delay = std::max(5, speedCombo_->currentData().toInt() / 10);
    if (!writeAnimatedGif(path, frames, delay, &error)) {
        QMessageBox::warning(this, QStringLiteral("Export failed"), error);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Animated timeline exported to %1").arg(path), 5000);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

} // namespace cappedweather
