#include "app/main_window.hpp"

#include "app/nexrad_source.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/weather_overlay_source.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"

#include <QComboBox>
#include <QCheckBox>
#include <QCompleter>
#include <QEvent>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QScreen>
#include <QSlider>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstddef>
#include <algorithm>
#include <cmath>
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

    connect(map_, &RadarMapCanvas::radarClicked, this, [this](const QString& radarId) {
        const auto row = model_->rowForRadar(radarId);
        if (row >= 0) {
            radarSelected(model_->index(row, 0));
        }
    });
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
        followLatest_ = index == static_cast<int>(history_.size()) - 1 && liveButton_->isChecked();
        showFrame(index);
    });
    connect(previousButton_, &QToolButton::clicked, this, [this] {
        stopAnimation();
        followLatest_ = false;
        showFrame(std::max(0, frameIndex_ - 1));
    });
    connect(nextButton_, &QToolButton::clicked, this, [this] {
        stopAnimation();
        followLatest_ = frameIndex_ + 1 >= static_cast<int>(history_.size()) - 1 && liveButton_->isChecked();
        showFrame(std::min(static_cast<int>(history_.size()) - 1, frameIndex_ + 1));
    });
    connect(playButton_, &QToolButton::toggled, this, [this](const bool playing) {
        if (!playing || history_.size() < 2) {
            if (playing) ensureHistoryLoaded(true);
            stopAnimation();
            return;
        }
        followLatest_ = false;
        liveButton_->setChecked(false);
        if (frameIndex_ >= static_cast<int>(history_.size()) - 1) showFrame(0);
        playButton_->setText(QStringLiteral("Pause"));
        animationTimer_->start(speedCombo_->currentData().toInt());
    });
    connect(liveButton_, &QToolButton::toggled, this, [this](const bool live) {
        followLatest_ = live;
        if (live && !history_.empty()) showFrame(static_cast<int>(history_.size()) - 1);
    });
    connect(speedCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (animationTimer_->isActive()) animationTimer_->setInterval(speedCombo_->currentData().toInt());
    });
    connect(warningsCheck_, &QCheckBox::toggled, map_, &RadarMapCanvas::setWarningsVisible);
    connect(reportsCheck_, &QCheckBox::toggled, map_, &RadarMapCanvas::setStormReportsVisible);
    connect(source_, &NexradSource::statusChanged, this, &MainWindow::sourceStatusChanged);
    connect(source_, &NexradSource::requestFailed, this, &MainWindow::sourceFailed);
    connect(source_, &NexradSource::catalogReceived, this, &MainWindow::catalogReceived);
    connect(source_, &NexradSource::volumeStarted, this, &MainWindow::volumeStarted);
    connect(source_, &NexradSource::volumeProgress, this, &MainWindow::volumeProgress);
    connect(source_, &NexradSource::chunkDecompressed, this, &MainWindow::chunkDecompressed);
    connect(source_, &NexradSource::volumeReady, this, &MainWindow::volumeReady);
    connect(overlaySource_, &WeatherOverlaySource::overlaysReady, this,
            [this](QVector<WeatherWarning> warnings, QVector<StormReport> reports, const QDateTime&) {
                map_->setWeatherOverlays(std::move(warnings), std::move(reports));
            });
    connect(overlaySource_, &WeatherOverlaySource::statusChanged, this, [this](const QString& status) {
        overlayStatus_ = status;
        updateUnifiedStatus();
    });

    connect(retryButton_, &QToolButton::clicked, this, [this] {
        retryButton_->hide();
        radarRequestActive_ = true;
        if (historyRequested_) {
            historyLoadStarted_ = true;
            source_->requestRecent(selectedRadarId_, 5);
        } else {
            source_->requestLatest(selectedRadarId_);
        }
    });
    connect(cancelButton_, &QToolButton::clicked, this, [this] {
        source_->cancel(selectedRadarId_);
        radarRequestActive_ = false;
        playWhenHistoryReady_ = false;
        map_->setLoading(false);
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

    map_->setSites(model_->sites());
    if (model_->rowCount() > 0) {
        auto row = model_->rowForRadar(settings.value(QStringLiteral("lastRadar")).toString());
        if (row < 0) row = 0;
        radarSelected(model_->index(row, 0));
    }
    source_->requestCatalog();
    refreshTimer_->start(60000);
    overlaySource_->refresh();
    overlayRefreshTimer_->start(60000);
    if (const auto* screen = QGuiApplication::primaryScreen()) {
        const auto available = screen->availableGeometry();
        const auto width = std::clamp(static_cast<int>(available.width() * 0.78), 900, 1180);
        const auto height = std::clamp(static_cast<int>(available.height() * 0.82), 620, 760);
        resize(std::min(width, available.width()), std::min(height, available.height()));
    } else {
        resize(1024, 680);
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

    map_ = new RadarMapCanvas(root);

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
    connect(nationalButton, &QToolButton::clicked, map_, &RadarMapCanvas::showNationalView);
    connect(recenterButton, &QToolButton::clicked, map_, &RadarMapCanvas::recenterSelected);
    connect(zoomOutButton, &QToolButton::clicked, map_, &RadarMapCanvas::zoomOut);
    connect(zoomInButton, &QToolButton::clicked, map_, &RadarMapCanvas::zoomIn);

    measureButton_ = addMapButton(QStringLiteral("Measure"), QStringLiteral("Measure distance and bearing"));
    measureButton_->setCheckable(true);
    connect(measureButton_, &QToolButton::toggled, map_, &RadarMapCanvas::setMeasureMode);

    viewControls->addSpacing(8);
    warningFilterCombo_ = new QComboBox(root);
    warningFilterCombo_->addItems({QStringLiteral("All warnings"), QStringLiteral("Tornado"),
                                   QStringLiteral("Severe Thunderstorm"), QStringLiteral("Flash Flood"),
                                   QStringLiteral("Extreme")});
    warningFilterCombo_->setToolTip(QStringLiteral("Filter visible warning polygons"));
    viewControls->addWidget(warningFilterCombo_);
    connect(warningFilterCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        map_->setWarningFilter(index == 0 ? QString{} : warningFilterCombo_->itemText(index));
    });
    tornadoCheck_ = new QCheckBox(QStringLiteral("Tornado"), root);
    hailCheck_ = new QCheckBox(QStringLiteral("Hail"), root);
    windCheck_ = new QCheckBox(QStringLiteral("Wind"), root);
    for (auto* check : {tornadoCheck_, hailCheck_, windCheck_}) check->setChecked(true);
    viewControls->addWidget(tornadoCheck_);
    viewControls->addWidget(hailCheck_);
    viewControls->addWidget(windCheck_);
    connect(tornadoCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        map_->setStormReportKindVisible(StormReportKind::Tornado, visible);
    });
    connect(hailCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        map_->setStormReportKindVisible(StormReportKind::Hail, visible);
    });
    connect(windCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        map_->setStormReportKindVisible(StormReportKind::Wind, visible);
    });

    viewControls->addStretch(1);
    paletteCombo_ = new QComboBox(root);
    paletteCombo_->addItems({QStringLiteral("Capped palette"), QStringLiteral("Operational palette"),
                             QStringLiteral("Color-safe palette")});
    viewControls->addWidget(paletteCombo_);
    connect(paletteCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        map_->setPalette(static_cast<RadarMapCanvas::PaletteKind>(index));
    });
    radarOpacitySlider_ = new QSlider(Qt::Horizontal, root);
    radarOpacitySlider_->setRange(20, 100);
    radarOpacitySlider_->setValue(80);
    radarOpacitySlider_->setFixedWidth(90);
    radarOpacitySlider_->setToolTip(QStringLiteral("Radar opacity"));
    viewControls->addWidget(radarOpacitySlider_);
    connect(radarOpacitySlider_, &QSlider::valueChanged, map_, &RadarMapCanvas::setRadarOpacity);
    belowThresholdCheck_ = new QCheckBox(QStringLiteral("Below threshold"), root);
    viewControls->addWidget(belowThresholdCheck_);
    connect(belowThresholdCheck_, &QCheckBox::toggled, map_, &RadarMapCanvas::setBelowThresholdVisible);
    rootLayout->addLayout(viewControls);

    rootLayout->addWidget(map_, 1);

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

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == frameSlider_ && event->type() == QEvent::MouseButtonPress && history_.size() < 2) {
        ensureHistoryLoaded(false);
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::radarSelected(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    const auto radarId = index.data(RadarListModel::RadarIdRole).toString();
    if (radarId == selectedRadarId_) {
        map_->recenterSelected();
        return;
    }
    const auto previousRadarId = selectedRadarId_;
    selectedRadarId_ = radarId;
    if (!previousRadarId.isEmpty()) source_->cancel(previousRadarId);
    recentRadarIds_.removeAll(radarId);
    recentRadarIds_.prepend(radarId);
    while (recentRadarIds_.size() > 8) recentRadarIds_.removeLast();
    QSettings settings;
    settings.setValue(QStringLiteral("recentRadars"), recentRadarIds_);
    settings.setValue(QStringLiteral("lastRadar"), radarId);
    rebuildStationPicker();
    stopAnimation();
    history_.clear();
    frameIndex_ = -1;
    followLatest_ = true;
    radarRequestActive_ = true;
    historyRequested_ = false;
    historyLoadStarted_ = false;
    playWhenHistoryReady_ = false;
    liveButton_->setChecked(true);
    updateProductAvailability();
    updatePlaybackControls();
    map_->setSelectedRadar(radarId);
    map_->setLoading(true, QStringLiteral("Loading %1 radar data").arg(radarId));
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
    source_->clearDownloaded(radarId);
    source_->requestLatest(radarId);
}

void MainWindow::requestSelectedRadar() {
    const auto radarId = selectedRadarId();
    if (!radarId.isEmpty()) {
        source_->requestLatest(radarId);
    }
}

void MainWindow::sourceStatusChanged(const QString& radarId, const QString& status) {
    model_->setStatus(radarId, status);
    if (radarId == selectedRadarId()) {
        if (status == QStringLiteral("no data") || status == QStringLiteral("no complete volume") ||
            status == QStringLiteral("up to date") || status == QStringLiteral("history loaded") ||
            status.startsWith(QStringLiteral("received "))) {
            radarRequestActive_ = false;
        }
        radarStatus_ = QStringLiteral("%1 %2").arg(radarId, status);
        statusLabel_->setText(radarStatus_);
        updateStationSummary();
        updateUnifiedStatus();
        if (status == QStringLiteral("no data") || status == QStringLiteral("no complete volume")) {
            map_->setLoading(false);
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
    if (radarId == selectedRadarId()) {
        radarRequestActive_ = false;
        radarStatus_ = QStringLiteral("%1 error: %2").arg(radarId, message);
        updateStationSummary();
        updateUnifiedStatus();
        map_->setLoading(false);
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
    map_->setSites(model_->sites());
    rebuildStationPicker();
    radarStatus_ = QStringLiteral("Official NWS radar catalog loaded");
    updateUnifiedStatus();
    auto row = model_->rowForRadar(selectedRadarId_);
    if (row < 0 && model_->rowCount() > 0) {
        row = 0;
    }
    if (row >= 0) {
        radarSelected(model_->index(row, 0));
    }
}

void MainWindow::volumeStarted(const QString& radarId, const QString& volumeId, const int chunkCount) {
    if (radarId != selectedRadarId()) {
        return;
    }
    pendingVolumeId_ = volumeId;
    pendingVolume_ = {};
    pendingVolume_.radarId = radarId.toStdString();
    pendingChunkCount_ = chunkCount;
    pendingFailedChunks_ = 0;
    loadingProgress_->setRange(0, std::max(1, chunkCount));
    loadingProgress_->setValue(0);
    loadingProgress_->show();
    cancelButton_->show();
    if (history_.empty()) {
        map_->setLoading(true, QStringLiteral("Downloading and decoding %1").arg(radarId));
    }
    radarStatus_ = QStringLiteral("%1 assembling %2 real volume chunks").arg(radarId).arg(chunkCount);
    updateUnifiedStatus();
}

void MainWindow::volumeProgress(const QString& radarId, const QString& volumeId, const int completed,
                                const int total, const int decoded, const int failed) {
    if (radarId != selectedRadarId() || volumeId != pendingVolumeId_) return;
    pendingFailedChunks_ = failed;
    loadingProgress_->setRange(0, std::max(1, total));
    loadingProgress_->setValue(completed);
    radarStatus_ = QStringLiteral("%1 volume chunks %2/%3 · %4 decoded%5")
                       .arg(radarId).arg(completed).arg(total).arg(decoded)
                       .arg(failed > 0 ? QStringLiteral(" · %1 unavailable").arg(failed) : QString{});
    map_->setLoading(completed < total, QStringLiteral("%1  %2/%3 chunks").arg(radarId).arg(completed).arg(total));
    updateUnifiedStatus();
}

void MainWindow::chunkDecompressed(const QString& radarId, const QString& volumeId, const QString&,
                                   const QByteArray& data, const QDateTime&) {
    if (radarId != selectedRadarId() || volumeId != pendingVolumeId_) {
        return;
    }
    const auto* raw = reinterpret_cast<const std::byte*>(data.constData());
    const auto decoded = decodeMessage31Chunk(std::span<const std::byte>(raw, static_cast<std::size_t>(data.size())),
                                              radarId.toStdString());
    pendingVolume_.messageCount += decoded.messageCount;
    pendingVolume_.rays.insert(pendingVolume_.rays.end(), decoded.rays.begin(), decoded.rays.end());
}

void MainWindow::volumeReady(const QString& radarId, const QString& volumeId, const int chunkCount,
                             const int failedChunks, const QDateTime& receivedAt) {
    if (radarId != selectedRadarId() || volumeId != pendingVolumeId_) {
        return;
    }
    model_->setStatus(radarId, QStringLiteral("received"), receivedAt);
    const auto followLatest = history_.empty() || followLatest_;
    const auto selectedVolume = frameIndex_ >= 0 && frameIndex_ < static_cast<int>(history_.size())
                                    ? history_.at(static_cast<std::size_t>(frameIndex_)).volumeId
                                    : QString{};
    const auto duplicate = std::find_if(history_.begin(), history_.end(), [&volumeId](const RadarFrame& frame) {
        return frame.volumeId == volumeId;
    });
    if (duplicate == history_.end()) {
        history_.push_back({volumeId, std::move(pendingVolume_), receivedAt, chunkCount, failedChunks});
    }
    std::sort(history_.begin(), history_.end(), [](const RadarFrame& left, const RadarFrame& right) {
        return left.dataTime < right.dataTime;
    });
    constexpr std::size_t kMaximumHistory = 8;
    if (history_.size() > kMaximumHistory) {
        history_.erase(history_.begin(),
                       history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - kMaximumHistory));
    }
    pendingVolumeId_.clear();
    radarRequestActive_ = false;
    loadingProgress_->hide();
    cancelButton_->hide();
    retryButton_->hide();
    map_->setLoading(false);
    if (followLatest) {
        frameIndex_ = static_cast<int>(history_.size()) - 1;
    } else {
        const auto selected = std::find_if(history_.begin(), history_.end(), [&selectedVolume](const RadarFrame& frame) {
            return frame.volumeId == selectedVolume;
        });
        frameIndex_ = selected == history_.end() ? 0 : static_cast<int>(std::distance(history_.begin(), selected));
    }
    updatePlaybackControls();
    showFrame(frameIndex_);
    radarStatus_ = QStringLiteral("%1 · %2 real volume%3 · %4")
                       .arg(radarId).arg(history_.size())
                       .arg(history_.size() == 1 ? QString{} : QStringLiteral("s"))
                       .arg(failedChunks == 0 ? QStringLiteral("complete")
                                              : QStringLiteral("partial: %1 chunk%2 unavailable")
                                                    .arg(failedChunks)
                                                    .arg(failedChunks == 1 ? QString{} : QStringLiteral("s")));
    updateStationSummary();
    updateUnifiedStatus();
    if (historyRequested_ && !historyLoadStarted_) {
        QTimer::singleShot(0, this, [this] {
            if (!historyRequested_ || historyLoadStarted_ || selectedRadarId_.isEmpty()) return;
            historyLoadStarted_ = true;
            radarRequestActive_ = true;
            radarStatus_ = QStringLiteral("%1 loading timeline history").arg(selectedRadarId_);
            updateUnifiedStatus();
            source_->requestRecent(selectedRadarId_, 5);
        });
    }
    if (playWhenHistoryReady_ && history_.size() >= 2) {
        playWhenHistoryReady_ = false;
        QTimer::singleShot(0, this, [this] { playButton_->setChecked(true); });
    }
}

void MainWindow::ensureHistoryLoaded(const bool playWhenReady) {
    if (playWhenReady) playWhenHistoryReady_ = true;
    if (history_.size() >= 2) {
        if (playWhenReady && !playButton_->isChecked()) playButton_->setChecked(true);
        return;
    }
    historyRequested_ = true;
    radarStatus_ = QStringLiteral("%1 timeline history requested").arg(selectedRadarId_);
    updateUnifiedStatus();
    if (radarRequestActive_ || historyLoadStarted_ || selectedRadarId_.isEmpty()) return;
    historyLoadStarted_ = true;
    radarRequestActive_ = true;
    source_->requestRecent(selectedRadarId_, 5);
}

void MainWindow::rebuildTiltCombo() {
    struct TiltSummary {
        int number = 0;
        double angleTotal = 0.0;
        int rayCount = 0;
    };
    const auto previousElevation = tiltCombo_->currentData().isValid()
                                       ? tiltCombo_->currentData().toInt() : -1;
    std::vector<TiltSummary> tilts;
    if (frameIndex_ < 0 || frameIndex_ >= static_cast<int>(history_.size())) {
        tiltCombo_->clear();
        tiltCombo_->addItem(QStringLiteral("Waiting for decoded tilts"));
        tiltCombo_->setEnabled(false);
        map_->setElevationFilter(std::nullopt);
        return;
    }
    const auto& currentVolume = history_.at(static_cast<std::size_t>(frameIndex_)).chunk;
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
    if (index < 0 || index >= static_cast<int>(history_.size())) return;
    frameIndex_ = index;
    const QSignalBlocker blocker(frameSlider_);
    frameSlider_->setValue(frameIndex_);
    const auto& frame = history_.at(static_cast<std::size_t>(frameIndex_));
    map_->setDecodedChunk(frame.chunk, frame.dataTime);
    map_->setVolumeQuality(std::max(0, frame.chunkCount - frame.failedChunks),
                           frame.chunkCount, frame.failedChunks);
    updateProductAvailability();
    rebuildTiltCombo();
    const auto ageSeconds = std::max<qint64>(0, frame.dataTime.secsTo(QDateTime::currentDateTimeUtc()));
    const auto ageText = ageSeconds < 60 ? QStringLiteral("now")
                         : ageSeconds < 3600 ? QStringLiteral("%1m ago").arg(ageSeconds / 60)
                                             : QStringLiteral("%1h %2m ago")
                                                   .arg(ageSeconds / 3600)
                                                   .arg((ageSeconds % 3600) / 60);
    const auto isLatest = frameIndex_ == static_cast<int>(history_.size()) - 1;
    frameLabel_->setText(QStringLiteral("%1/%2 · %3 · %4%5")
                             .arg(frameIndex_ + 1).arg(history_.size())
                             .arg(frame.dataTime.toLocalTime().toString(QStringLiteral("HH:mm:ss")))
                             .arg(ageText)
                             .arg(isLatest && followLatest_ ? QStringLiteral(" · LIVE") : QString{}));
    frameSlider_->setToolTip(QStringLiteral("Frame %1 of %2\n%3\n%4")
                                 .arg(frameIndex_ + 1).arg(history_.size())
                                 .arg(frame.dataTime.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss t")))
                                 .arg(frame.failedChunks == 0 ? QStringLiteral("Complete volume")
                                                              : QStringLiteral("Partial: %1 chunks unavailable")
                                                                    .arg(frame.failedChunks)));
    {
        const QSignalBlocker liveBlocker(liveButton_);
        liveButton_->setChecked(followLatest_ && isLatest);
    }
    updatePlaybackControls();
}

void MainWindow::updateProductAvailability() {
    auto* itemModel = qobject_cast<QStandardItemModel*>(productCombo_->model());
    if (!itemModel) return;
    const auto hasFrame = frameIndex_ >= 0 && frameIndex_ < static_cast<int>(history_.size());
    int firstAvailable = -1;
    for (int row = 0; row < productCombo_->count(); ++row) {
        const auto product = static_cast<ProductKind>(productCombo_->itemData(row).toInt());
        const auto available = !hasFrame || std::any_of(
            history_.at(static_cast<std::size_t>(frameIndex_)).chunk.rays.cbegin(),
            history_.at(static_cast<std::size_t>(frameIndex_)).chunk.rays.cend(),
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
    if (history_.size() < 2) {
        stopAnimation();
        return;
    }
    followLatest_ = false;
    showFrame((frameIndex_ + 1) % static_cast<int>(history_.size()));
}

void MainWindow::updatePlaybackControls() {
    const auto hasFrames = !history_.empty();
    const auto canAnimate = history_.size() > 1;
    frameSlider_->setRange(0, std::max(0, static_cast<int>(history_.size()) - 1));
    previousButton_->setEnabled(hasFrames && frameIndex_ > 0);
    nextButton_->setEnabled(hasFrames && frameIndex_ + 1 < static_cast<int>(history_.size()));
    playButton_->setEnabled(hasFrames);
    frameSlider_->setEnabled(hasFrames);
    speedCombo_->setEnabled(canAnimate);
    liveButton_->setEnabled(hasFrames);
    if (!hasFrames) frameLabel_->setText(QStringLiteral("Waiting for latest volume"));
    else if (!canAnimate && !historyRequested_) {
        frameLabel_->setText(QStringLiteral("LIVE · click timeline or Play to load history"));
        frameSlider_->setToolTip(QStringLiteral("Click to load historical volumes"));
    }
}

void MainWindow::stopAnimation() {
    animationTimer_->stop();
    if (playButton_->isChecked()) playButton_->setChecked(false);
    playButton_->setText(QStringLiteral("Play"));
}

} // namespace cappedweather
