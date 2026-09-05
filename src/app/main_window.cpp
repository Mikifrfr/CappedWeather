#include "app/main_window.hpp"

#include "app/nexrad_source.hpp"
#include "app/opera_source.hpp"
#include "app/glm_source.hpp"
#include "app/http_client.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/weather_overlay_source.hpp"
#include "app/placefile_source.hpp"
#include "app/archive_source.hpp"
#include "core/derived_products.hpp"
#include "app/cross_section_widget.hpp"
#include "app/gif_writer.hpp"
#include "app/shortcut_help_dialog.hpp"
#include "core/level2_decoder.hpp"
#include "core/glm_l2_reader.hpp"
#include "core/product.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCompleter>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QEventLoop>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QResizeEvent>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSlider>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QStatusBar>
#include <QThread>
#include <QThreadPool>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QTextStream>
#include <QUrlQuery>
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

namespace {

GlmLiveConfig glmConfigFromSettings() {
    QSettings settings;
    GlmLiveConfig config;
    config.refreshSeconds = settings.value(QStringLiteral("lightning/refreshSeconds"), 20).toInt();
    config.initialBackfillMinutes = settings.value(QStringLiteral("lightning/backfillMinutes"), 18).toInt();
    config.maximumConcurrentDownloads = settings.value(QStringLiteral("lightning/parallelDownloads"), 4).toInt();
    config.cessationRetentionMinutes = settings.value(QStringLiteral("lightning/cessationMinutes"), 10).toInt();
    config.grid.cellSizeDeg = settings.value(QStringLiteral("lightning/cellSizeDeg"), 0.25).toDouble();
    config.grid.jump.minFlashRatePerMin = settings.value(QStringLiteral("lightning/minFlashRate"), 12.0).toDouble();
    config.grid.jump.jumpSigmaThreshold = settings.value(QStringLiteral("lightning/sigmaThreshold"), 2.0).toDouble();
    config.grid.jump.sigmaWindowSec = settings.value(QStringLiteral("lightning/sigmaWindowMinutes"), 10.0).toDouble() * 60.0;
    config.grid.jump.rateResolutionSec = settings.value(QStringLiteral("lightning/rateResolutionSeconds"), 60.0).toDouble();
    config.grid.jump.minSigmaSamples = settings.value(QStringLiteral("lightning/minSigmaSamples"), 5).toInt();
    return config;
}

bool placefileUrlContainsCredentials(const QUrl& url) {    if (!url.userName().isEmpty() || !url.password().isEmpty()) return true;
    const auto items = QUrlQuery(url).queryItems(QUrl::FullyDecoded);
    return std::any_of(items.cbegin(), items.cend(), [](const auto& item) {
        const auto key = item.first.toLower();
        return key.contains(QStringLiteral("token")) || key.contains(QStringLiteral("secret")) ||
            key.contains(QStringLiteral("password")) || key == QStringLiteral("key") ||
            key.contains(QStringLiteral("api_key")) || key.contains(QStringLiteral("apikey")) ||
            key.contains(QStringLiteral("auth"));
    });
}

// Chip dot colors mirror the offtrack sidebar mockup.
QColor productChipColor(const ProductKind kind) {
    switch (kind) {
        case ProductKind::Reflectivity:
            return QColor(QStringLiteral("#22c55e"));
        case ProductKind::Velocity:
            return QColor(QStringLiteral("#38bdf8"));
        case ProductKind::SpectrumWidth:
            return QColor(QStringLiteral("#a78bfa"));
        case ProductKind::DifferentialReflectivity:
            return QColor(QStringLiteral("#f472b6"));
        case ProductKind::CorrelationCoefficient:
            return QColor(QStringLiteral("#facc15"));
        case ProductKind::DifferentialPhase:
            return QColor(QStringLiteral("#fb923c"));
        case ProductKind::EchoTop:
            return QColor(QStringLiteral("#94a3b8"));
        case ProductKind::VerticallyIntegratedLiquid:
            return QColor(QStringLiteral("#2dd4bf"));
    }
    return QColor(QStringLiteral("#8b94a5"));
}

QIcon productChipIcon(const ProductKind kind) {
    constexpr int extent = 14;
    QPixmap pixmap(extent, extent);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    if (kind == ProductKind::Velocity) {
        // Split red/blue for toward/away motion.
        painter.setBrush(QColor(QStringLiteral("#f87171")));
        painter.drawRoundedRect(0, 0, extent, extent, 3, 3);
        painter.setClipRect(extent / 2, 0, extent - extent / 2, extent);
        painter.setBrush(QColor(QStringLiteral("#38bdf8")));
        painter.drawRoundedRect(0, 0, extent, extent, 3, 3);
    } else {
        painter.setBrush(productChipColor(kind));
        painter.drawRoundedRect(0, 0, extent, extent, 3, 3);
    }
    return QIcon(pixmap);
}

// One-line operator guidance shown on product chip hover.
QString productChipTooltip(const ProductKind kind) {
    switch (kind) {
        case ProductKind::Reflectivity:
            return QStringLiteral("Reflectivity (dBZ). Bright cores mean heavy rain, hail, or debris.");
        case ProductKind::Velocity:
            return QStringLiteral("Motion toward or away from the radar (m/s). Tight red/green couplets mean rotation.");
        case ProductKind::SpectrumWidth:
            return QStringLiteral("Velocity spread inside one gate (m/s). High values mark turbulence and debris.");
        case ProductKind::DifferentialReflectivity:
            return QStringLiteral("Horizontal vs vertical return size (dB). Near zero in hail and debris.");
        case ProductKind::CorrelationCoefficient:
            return QStringLiteral("Target uniformity (unitless). Below 0.9 in debris, melting hail, clutter.");
        case ProductKind::DifferentialPhase:
            return QStringLiteral("Phase shift along the beam (deg). Its gradient tracks heavy rain.");
        case ProductKind::EchoTop:
            return QStringLiteral("Derived: height of the 18 dBZ echo (kft). Above 35 kft means a strong updraft.");
        case ProductKind::VerticallyIntegratedLiquid:
            return QStringLiteral("Derived: column water (kg/m2). High values with hail colors are dangerous.");
    }
    return {};
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      model_(new RadarListModel(this)),
      source_(new NexradSource(this)),
      operaSource_(new OperaSource(this)),
      glmSource_(new GlmSource(this)),
      overlaySource_(new WeatherOverlaySource(this)),
      placefileSource_(new PlacefileSource(this)),
      archiveSource_(new ArchiveSource(this)),
      alertCenter_(new AlertCenter(this)),
      refreshTimer_(new QTimer(this)),
      overlayRefreshTimer_(new QTimer(this)),
      animationTimer_(new QTimer(this)),
      healthTimer_(new QTimer(this)) {
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
        // Push the active panel's view to its undo stack. We only record when
        // the active panel itself changed (not when it's a passive recipient
        // of a sync) so the back stack represents user actions.
        connect(panel, &RadarMapCanvas::viewChanged, this,
                [this, panel](const QPointF&, int) {
                    if (synchronizingMapView_) return;
                    if (panel == map_) panel->pushViewHistory();
                });
        connect(panel, &RadarMapCanvas::crossSectionRequested, this,
                [this, panel](const QPointF& start, const QPointF& end) {
                    activateMap(panel);
                    const QSignalBlocker blocker(crossSectionButton_);
                    crossSectionButton_->setChecked(false);
                    panel->setCrossSectionMode(false);
                    showCrossSection(start, end);
                });
        connect(panel, &RadarMapCanvas::measureModeToggled, this,
                [this, panel](bool enabled) {
                    if (measureButton_) {
                        const QSignalBlocker blocker(measureButton_);
                        measureButton_->setChecked(enabled);
                    }
                });
        connect(panel, &RadarMapCanvas::crossSectionModeToggled, this,
                [this, panel](bool enabled) {
                    if (crossSectionButton_) {
                        const QSignalBlocker blocker(crossSectionButton_);
                        crossSectionButton_->setChecked(enabled);
                    }
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
        updateCrossSectionAvailability();
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
    healthTimer_->setInterval(30'000); // 30 s — enough resolution to flip Online/Stale
    healthTimer_->setSingleShot(false);
    connect(healthTimer_, &QTimer::timeout, this, &MainWindow::recomputeRadarHealth);
    healthTimer_->start();
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
        for (auto* panel : maps_) {
            if (!panel->overlayOverrides().has_value()) panel->setWarningsVisible(visible);
        }
    });
    connect(reportsCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) {
            if (!panel->overlayOverrides().has_value()) panel->setStormReportsVisible(visible);
        }
    });
    connect(lightningCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) {
            if (!panel->overlayOverrides().has_value()) panel->setLightningVisible(visible);
        }
        lightningFusionCheck_->setEnabled(visible);
        refreshLayerBadges();
    });
    connect(lightningFusionCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) {
            if (!panel->overlayOverrides().has_value()) panel->setLightningFusionVisible(visible);
        }
        refreshLayerBadges();
    });
    connect(placefilesCheck_, &QCheckBox::toggled, this, [this](const bool visible) {
        for (auto* panel : maps_) panel->setPlacefilesVisible(visible);
    });
    connect(addPlacefileButton_, &QToolButton::clicked, this, &MainWindow::openLocalPlacefile);
    connect(addPlacefileUrlButton_, &QToolButton::clicked, this, &MainWindow::addPlacefileUrl);
    connect(clearPlacefilesButton_, &QToolButton::clicked, this, &MainWindow::clearPlacefiles);
    connect(placefileSource_, &PlacefileSource::documentsChanged, this,
            [this](QVector<PlacefileDocument> documents) {
                for (auto* panel : maps_) panel->setPlacefiles(documents);
                clearPlacefilesButton_->setEnabled(!documents.isEmpty());
                refreshLayerBadges();
            });
    connect(placefileSource_, &PlacefileSource::statusChanged, this, [this](const QString& status) {
        statusBar()->showMessage(status, 8000);
    });
    connect(source_, &NexradSource::statusChanged, this, &MainWindow::sourceStatusChanged);
    connect(source_, &NexradSource::requestFailed, this, &MainWindow::sourceFailed);
    connect(source_, &NexradSource::catalogReceived, this, &MainWindow::catalogReceived);
    connect(source_, &NexradSource::volumeStarted, this, &MainWindow::volumeStarted);
    connect(source_, &NexradSource::volumeProgress, this, &MainWindow::volumeProgress);
    connect(source_, &NexradSource::chunkDecoded, this, &MainWindow::chunkDecoded);
    connect(source_, &NexradSource::volumeReady, this, &MainWindow::volumeReady);
    connect(operaSource_, &OperaSource::statusChanged, this, &MainWindow::sourceStatusChanged);
    connect(operaSource_, &OperaSource::requestFailed, this, &MainWindow::sourceFailed);
    connect(operaSource_, &OperaSource::volumeStarted, this, &MainWindow::volumeStarted);
    connect(operaSource_, &OperaSource::volumeProgress, this, &MainWindow::volumeProgress);
    connect(operaSource_, &OperaSource::chunkDecoded, this, &MainWindow::chunkDecoded);
    connect(operaSource_, &OperaSource::volumeReady, this, &MainWindow::volumeReady);
    connect(archiveSource_, &ArchiveSource::statusChanged, this, &MainWindow::sourceStatusChanged);
    connect(archiveSource_, &ArchiveSource::requestFailed, this,
            [this](const QString& radarId, const QString& message) {
                if (radarId != selectedRadarId()) return;
                radarStatus_ = QStringLiteral("%1 archive: %2").arg(radarId, message);
                updateUnifiedStatus();
                statusBar()->showMessage(radarStatus_, 8000);
            });
    connect(archiveSource_, &ArchiveSource::volumeStarted, this, &MainWindow::volumeStarted);
    connect(archiveSource_, &ArchiveSource::volumeProgress, this, &MainWindow::volumeProgress);
    connect(archiveSource_, &ArchiveSource::chunkDecoded, this, &MainWindow::chunkDecoded);
    connect(archiveSource_, &ArchiveSource::volumeReady, this, &MainWindow::volumeReady);
    connect(overlaySource_, &WeatherOverlaySource::overlaysReady, this,
            [this](QVector<WeatherWarning> warnings, QVector<StormReport> reports, const QDateTime&) {
                for (auto* panel : maps_) panel->setWeatherOverlays(warnings, reports);
                if (alertCenter_) alertCenter_->ingestWarnings(warnings);
                refreshLayerBadges();
            });
    connect(overlaySource_, &WeatherOverlaySource::statusChanged, this, [this](const QString& status) {
        overlayStatus_ = status;
        updateUnifiedStatus();
    });
    connect(glmSource_, &GlmSource::cellsUpdated, this,
            [this](QVector<LightningCellSnapshot> cells, const QDateTime&, int, int) {
                std::vector<LightningCellSnapshot> snapshots(cells.begin(), cells.end());
                for (auto* panel : maps_) panel->setLightningCells(snapshots);
                if (alertCenter_) alertCenter_->ingestLightningCells(snapshots);
                refreshLayerBadges();
            });
    connect(glmSource_, &GlmSource::statusChanged, this, [this](const QString& status) {
        lightningStatus_ = status;
        updateUnifiedStatus();
    });
    connect(glmSource_, &GlmSource::requestFailed, this, [this](const QString& message) {
        lightningStatus_ = message;
        updateUnifiedStatus();
    });

    connect(retryButton_, &QToolButton::clicked, this, [this] {
        const auto session = activeSession();
        if (!session) return;
        retryButton_->hide();
        session->requestActive = true;
        if (session->historyRequested) {
            session->historyLoadStarted = true;
            requestRecentFor(selectedRadarId_, historyVolumeCount_);
        } else {
            requestLatestFor(selectedRadarId_);
        }
    });
    connect(cancelButton_, &QToolButton::clicked, this, [this] {
        const auto session = activeSession();
        cancelFor(selectedRadarId_);
        archiveSource_->cancel();
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
    addShortcut(QKeySequence(Qt::Key_Home), [this] {
        const auto session = activeSession();
        if (!session || session->history.empty()) return;
        stopAnimation();
        session->followLatest = false;
        showFrame(0);
    });
    addShortcut(QKeySequence(Qt::Key_End), [this] {
        const auto session = activeSession();
        if (!session || session->history.empty()) return;
        session->followLatest = liveButton_->isChecked();
        showFrame(static_cast<int>(session->history.size()) - 1);
    });
    addShortcut(QKeySequence(Qt::Key_P), [this] {
        if (!map_) return;
        map_->setPinnedOverlayLocked(!map_->pinnedOverlayLocked());
        updateUnifiedStatus();
    });
    addShortcut(QKeySequence::Undo, [this] {
        if (map_ && map_->canUndoView()) {
            map_->undoView();
            statusBar()->showMessage(QStringLiteral("View undo (%1 left)").arg(map_->viewBackSizeForUi()), 3000);
        }
    });
    addShortcut(QKeySequence::Redo, [this] {
        if (map_ && map_->canRedoView()) {
            map_->redoView();
            statusBar()->showMessage(QStringLiteral("View redo (%1 forward)").arg(map_->viewForwardSizeForUi()), 3000);
        }
    });
    connect(timeJumpEdit_, &QDateTimeEdit::editingFinished, this, [this] {
        const auto session = activeSession();
        if (!session || session->history.empty()) return;
        const auto target = timeJumpEdit_->dateTime();
        if (target <= timeJumpEdit_->minimumDateTime()) return; // special value
        int bestIndex = 0;
        qint64 bestDelta = std::numeric_limits<qint64>::max();
        for (std::size_t i = 0; i < session->history.size(); ++i) {
            const auto dt = session->history[i].dataTime;
            if (!dt.isValid()) continue;
            const auto delta = std::abs(dt.toSecsSinceEpoch() - target.toSecsSinceEpoch());
            if (delta < bestDelta) { bestDelta = delta; bestIndex = static_cast<int>(i); }
        }
        stopAnimation();
        session->followLatest = false;
        showFrame(bestIndex);
    });
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
    addShortcut(QKeySequence(QStringLiteral("?")), [this] {
        ShortcutHelpDialog dialog(this);
        dialog.exec();
    });
    addShortcut(QKeySequence(Qt::Key_F1), [this] {
        ShortcutHelpDialog dialog(this);
        dialog.exec();
    });

    restoreSettings();
    remotePlacefileUrls_ = QSettings().value(QStringLiteral("placefiles/urls")).toStringList();
    remotePlacefileUrls_.erase(
        std::remove_if(remotePlacefileUrls_.begin(), remotePlacefileUrls_.end(),
                       [](const QString& value) { return placefileUrlContainsCredentials(QUrl(value)); }),
        remotePlacefileUrls_.end());
    QSettings().setValue(QStringLiteral("placefiles/urls"), remotePlacefileUrls_);

    for (auto* panel : maps_) panel->setSites(model_->sites());
    if (model_->rowCount() > 0) {
        const auto primaryRadar = settings.value(
            QStringLiteral("lastRadar"), settings.value(QStringLiteral("panels/radar0"))).toString();
        auto row = chaseMode_ ? -1 : model_->rowForRadar(primaryRadar);
        if (row < 0) row = 0;
        radarSelected(model_->index(row, 0));
        if (chaseMode_) selectNearestChaseRadar();
    }
    for (const auto& sourceUrl : remotePlacefileUrls_) placefileSource_->addUrl(QUrl(sourceUrl));
    source_->requestCatalog();
    refreshTimer_->start(std::clamp(settings.value(QStringLiteral("network/radarRefreshSeconds"), 60).toInt(),
                                    30, 600) * 1000);
    overlaySource_->refresh();
    overlayRefreshTimer_->start(std::clamp(settings.value(QStringLiteral("network/overlayRefreshSeconds"), 60).toInt(),
                                           30, 600) * 1000);
    glmSource_->configure(glmConfigFromSettings());
    glmSource_->start();
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
    const auto dockState = settings.value(QStringLiteral("window/dockState")).toByteArray();
    if (!dockState.isEmpty()) restoreState(dockState, 1);
    const auto maximumDockWidth = std::max(220, width() - 420);
    const auto storedDockWidth = std::clamp(
        settings.value(QStringLiteral("window/controlDockWidth"), 272).toInt(),
        220, maximumDockWidth);
    resizeDocks({controlDock_}, {storedDockWidth}, Qt::Horizontal);
}

void MainWindow::applyTheme(const AppTheme theme) {
    theme_ = theme;
    QGuiApplication::setPalette(appPalette(theme));
    qApp->setStyleSheet(appStylesheet(theme));
    if (mapGrid_ != nullptr) mapGrid_->setSpacing(theme == AppTheme::Prototype ? 6 : 4);
    const auto accent = appAccent(theme);
    for (auto* panel : maps_) {
        if (panel != nullptr) panel->setActiveBorderColor(accent);
    }
    update();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("CappedWeather - Level II Radar"));

    QFont uiFont = QGuiApplication::font();
    uiFont.setFamilies({QStringLiteral("IBM Plex Sans"), QStringLiteral("Segoe UI"),
                        QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans")});
    QGuiApplication::setFont(uiFont);
    QFont monoFont = uiFont;
    monoFont.setFamilies({QStringLiteral("IBM Plex Mono"), QStringLiteral("Cascadia Mono"),
                          QStringLiteral("Consolas"), QStringLiteral("monospace")});

    applyTheme(theme_);

    auto* mapHost = new QWidget(this);
    mapHost->setObjectName(QStringLiteral("mapHost"));
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
    setCentralWidget(mapHost);

    emptyStateHint_ = new QLabel(mapHost);
    emptyStateHint_->setObjectName(QStringLiteral("emptyStateHint"));
    emptyStateHint_->setText(QStringLiteral(
        "Welcome to CappedWeather — pick a radar to begin. "
        "Tip: Station list → LIVE → timeline → overlays."));
    emptyStateHint_->setAlignment(Qt::AlignCenter);
    emptyStateHint_->setWordWrap(true);
    emptyStateHint_->setStyleSheet(QStringLiteral(
        "color: #d4d6d9; background: rgba(26,27,30,200); padding: 10px; border-radius: 6px;"));
    emptyStateHint_->raise();
    emptyStateHint_->show();

    controlDock_ = new QDockWidget(QStringLiteral("RADAR"), this);
    controlDock_->setObjectName(QStringLiteral("controlDock"));
    controlDock_->setFeatures(QDockWidget::DockWidgetMovable);
    controlDock_->setMinimumWidth(300);
    auto* controlScroll = new QScrollArea(controlDock_);
    controlScroll->setWidgetResizable(true);
    controlScroll->setFrameShape(QFrame::NoFrame);
    controlScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* controlBody = new QWidget(controlScroll);
    controlBody->setObjectName(QStringLiteral("dockBody"));
    auto* controlLayout = new QVBoxLayout(controlBody);
    controlLayout->setContentsMargins(12, 8, 12, 12);
    controlLayout->setSpacing(4);
    const auto makeGroup = [this, controlBody, controlLayout](const QString& title,
                                                              const QString& key) {
        auto* header = new QToolButton(controlBody);
        header->setObjectName(QStringLiteral("groupHeader"));
        header->setText(title);
        header->setCheckable(true);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        auto* body = new QWidget(controlBody);
        auto* bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 2, 0, 8);
        bodyLayout->setSpacing(5);
        controlLayout->addWidget(header);
        controlLayout->addWidget(body);
        const bool collapsed =
            QSettings().value(QStringLiteral("ui/collapsed/") + key, false).toBool();
        header->setChecked(!collapsed);
        header->setArrowType(header->isChecked() ? Qt::DownArrow : Qt::RightArrow);
        body->setVisible(!collapsed);
        connect(header, &QToolButton::toggled, this, [header, body, key](bool expanded) {
            header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            body->setVisible(expanded);
            QSettings().setValue(QStringLiteral("ui/collapsed/") + key, !expanded);
        });
        return bodyLayout;
    };
    const auto makeBadge = [controlBody](const QString& name) {
        auto* badge = new QLabel(QStringLiteral("–"), controlBody);
        badge->setObjectName(name);
        return badge;
    };

    // Pinned station status card: selected station + data health stay visible
    // even when every group below is collapsed.
    auto* stationCard = new QFrame(controlBody);
    stationCard->setObjectName(QStringLiteral("stationCard"));
    auto* stationCardLayout = new QVBoxLayout(stationCard);
    stationCardLayout->setContentsMargins(10, 8, 10, 8);
    stationCardLayout->setSpacing(3);
    controlLayout->addWidget(stationCard);
    stationSummaryLabel_ = new QLabel(QStringLiteral("Select a radar"), stationCard);
    stationSummaryLabel_->setObjectName(QStringLiteral("section"));
    stationSummaryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    stationSummaryLabel_->setMinimumWidth(0);
    stationSummaryLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stationCardLayout->addWidget(stationSummaryLabel_);
    auto* healthRow = new QHBoxLayout();
    healthRow->setObjectName(QStringLiteral("healthRow"));
    healthDot_ = new QLabel(stationCard);
    healthDot_->setObjectName(QStringLiteral("healthDot"));
    healthDot_->setFixedSize(12, 12);
    healthDot_->setToolTip(QStringLiteral("Radar data health"));
    healthRow->addWidget(healthDot_);
    healthLabel_ = new QLabel(QStringLiteral("Unknown"), stationCard);
    healthLabel_->setObjectName(QStringLiteral("healthLabel"));
    healthLabel_->setToolTip(QStringLiteral("Reflects whether real Level II chunks are arriving for the selected radar"));
    healthRow->addWidget(healthLabel_, 1);
    stationCardLayout->addLayout(healthRow);
    setRadarHealth(RadarHealthState::Unknown, QStringLiteral("Pick a station to begin"));
    statusLabel_ = new QLabel(controlBody);
    statusLabel_->setObjectName(QStringLiteral("section"));
    statusLabel_->hide();
    controlLayout->addWidget(statusLabel_);

    auto* radarGroup = makeGroup(QStringLiteral("Radar"), QStringLiteral("radar"));
    auto* stationRow = new QHBoxLayout();
    stationRow->setSpacing(4);
    stationCombo_ = new QComboBox(controlBody);
    stationCombo_->setObjectName(QStringLiteral("stationCombo"));
    stationCombo_->setEditable(true);
    stationCombo_->setInsertPolicy(QComboBox::NoInsert);
    stationCombo_->setMinimumContentsLength(24);
    stationCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    stationCombo_->lineEdit()->setPlaceholderText(QStringLiteral("Find radar, city, or state…"));
    stationCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    stationRow->addWidget(stationCombo_, 1);
    favoriteButton_ = new QToolButton(controlBody);
    favoriteButton_->setObjectName(QStringLiteral("favoriteRadarButton"));
    favoriteButton_->setText(QStringLiteral("☆"));
    favoriteButton_->setCheckable(true);
    favoriteButton_->setToolTip(QStringLiteral("Favorite selected radar"));
    stationRow->addWidget(favoriteButton_);
    radarGroup->addLayout(stationRow);

    auto* productGroup = makeGroup(QStringLiteral("Product and tilt"), QStringLiteral("product"));
    // The combo stays the data owner (logic, persistence, tests address it);
    // the chip grid is its visible face. It is hidden, never removed.
    productCombo_ = new QComboBox(controlBody);
    productCombo_->setObjectName(QStringLiteral("productCombo"));
    for (const auto& product : productDescriptors()) {
        productCombo_->addItem(QString::fromUtf8(product.label.data()), static_cast<int>(product.kind));
    }
    productCombo_->hide();
    auto* chipGrid = new QGridLayout();
    chipGrid->setSpacing(5);
    chipGrid->setObjectName(QStringLiteral("productChipGrid"));
    int chipRow = 0;
    int chipColumn = 0;
    for (const auto& product : productDescriptors()) {
        const auto kind = product.kind;
        auto* chip = new QToolButton(controlBody);
        chip->setObjectName(QStringLiteral("productChip"));
        chip->setText(QString::fromUtf8(product.label.data()));
        chip->setIcon(productChipIcon(kind));
        chip->setIconSize(QSize(14, 14));
        chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        chip->setToolTip(productChipTooltip(kind));
        chip->setCheckable(true);
        chip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        chip->setProperty("productKind", static_cast<int>(kind));
        connect(chip, &QToolButton::clicked, this, [this, kind] {
            const auto index = productCombo_->findData(static_cast<int>(kind));
            if (index >= 0) productCombo_->setCurrentIndex(index);
        });
        chipGrid->addWidget(chip, chipRow, chipColumn);
        productChips_.push_back(chip);
        if (++chipColumn > 1) {
            chipColumn = 0;
            ++chipRow;
        }
    }
    productGroup->addLayout(chipGrid);
    connect(productCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { syncProductChips(); });
    // Tilt stepper + expandable tilt grid. tiltCombo_ stays the data owner.
    tiltCombo_ = new QComboBox(controlBody);
    tiltCombo_->setObjectName(QStringLiteral("tiltCombo"));
    tiltCombo_->addItem(QStringLiteral("Latest available tilt"));
    tiltCombo_->setEnabled(false);
    tiltCombo_->hide();
    auto* tiltStepper = new QHBoxLayout();
    tiltStepper->setSpacing(5);
    tiltMinus_ = new QToolButton(controlBody);
    tiltMinus_->setObjectName(QStringLiteral("tiltMinusButton"));
    tiltMinus_->setText(QStringLiteral("−"));
    tiltMinus_->setToolTip(QStringLiteral("Previous tilt"));
    tiltPlus_ = new QToolButton(controlBody);
    tiltPlus_->setObjectName(QStringLiteral("tiltPlusButton"));
    tiltPlus_->setText(QStringLiteral("+"));
    tiltPlus_->setToolTip(QStringLiteral("Next tilt"));
    tiltValueButton_ = new QToolButton(controlBody);
    tiltValueButton_->setObjectName(QStringLiteral("tiltValueButton"));
    tiltValueButton_->setToolTip(QStringLiteral("Click for the full tilt list"));
    tiltValueButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tiltStepper->addWidget(tiltMinus_);
    tiltStepper->addWidget(tiltValueButton_, 1);
    tiltStepper->addWidget(tiltPlus_);
    productGroup->addLayout(tiltStepper);
    tiltGridBox_ = new QWidget(controlBody);
    tiltGridBox_->setObjectName(QStringLiteral("tiltGrid"));
    auto* tiltGridLayout = new QGridLayout(tiltGridBox_);
    tiltGridLayout->setContentsMargins(0, 0, 0, 0);
    tiltGridLayout->setSpacing(4);
    productGroup->addWidget(tiltGridBox_);
    tiltGridBox_->hide();
    connect(tiltMinus_, &QToolButton::clicked, this, [this] {
        if (tiltCombo_->isEnabled() && tiltCombo_->currentIndex() > 0)
            tiltCombo_->setCurrentIndex(tiltCombo_->currentIndex() - 1);
    });
    connect(tiltPlus_, &QToolButton::clicked, this, [this] {
        if (tiltCombo_->isEnabled() && tiltCombo_->currentIndex() + 1 < tiltCombo_->count())
            tiltCombo_->setCurrentIndex(tiltCombo_->currentIndex() + 1);
    });
    connect(tiltValueButton_, &QToolButton::clicked, this, [this] {
        tiltGridBox_->setVisible(!tiltGridBox_->isVisible());
    });
    connect(tiltCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { rebuildTiltGrid(); });
    syncProductChips();
    // Velocity processing only applies to the Velocity product, so its
    // controls live inside the product group and appear contextually.
    velocityBox_ = new QWidget(controlBody);
    velocityBox_->setObjectName(QStringLiteral("velocityBox"));
    auto* velocityBoxLayout = new QVBoxLayout(velocityBox_);
    velocityBoxLayout->setContentsMargins(0, 0, 0, 0);
    velocityBoxLayout->setSpacing(4);
    auto* velocityCaption = new QLabel(QStringLiteral("Velocity processing"), controlBody);
    velocityCaption->setObjectName(QStringLiteral("groupCaption"));
    velocityBoxLayout->addWidget(velocityCaption);
    productGroup->addWidget(velocityBox_);
    velocityBox_->hide();

    auto* layersGroup = makeGroup(QStringLiteral("Layers"), QStringLiteral("layers"));
    auto* scopeRow = new QHBoxLayout();
    auto* scopeLabel = new QLabel(QStringLiteral("Show layers on"), controlBody);
    scopeCombo_ = new QComboBox(controlBody);
    scopeCombo_->setObjectName(QStringLiteral("scopeCombo"));
    scopeCombo_->addItems({QStringLiteral("All panels"), QStringLiteral("Active panel only")});
    scopeCombo_->setToolTip(QStringLiteral("Whether layer toggles below drive every panel or just the active one"));
    scopeRow->addWidget(scopeLabel);
    scopeRow->addWidget(scopeCombo_, 1);
    layersGroup->addLayout(scopeRow);
    connect(scopeCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        QSettings().setValue(QStringLiteral("ui/layerScope"), scopeCombo_->currentIndex());
        refreshScopeVisibility();
    });
    // One row per layer: toggle + live count badge. The checkboxes stay the
    // data owners (logic, persistence, tests address them).
    const auto makeLayerRow = [controlBody, layersGroup, makeBadge](
                                  QCheckBox*& check, const QString& name, const QString& object,
                                  const QString& sub, QLabel*& badge, const QString& badgeName,
                                  bool checked, const QString& tip) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);
        check = new QCheckBox(name, controlBody);
        check->setObjectName(object);
        check->setChecked(checked);
        if (!tip.isEmpty()) check->setToolTip(tip);
        check->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        row->addWidget(check, 1);
        badge = makeBadge(badgeName);
        row->addWidget(badge);
        layersGroup->addLayout(row);
        if (!sub.isEmpty()) {
            auto* subLabel = new QLabel(sub, controlBody);
            subLabel->setObjectName(QStringLiteral("layerSub"));
            layersGroup->addWidget(subLabel);
        }
    };
    makeLayerRow(warningsCheck_, QStringLiteral("Warnings"), QStringLiteral("warningsCheck"),
                 QStringLiteral("Tornado, severe, flash flood"), warningsBadge_,
                 QStringLiteral("warningsBadge"), true, {});
    makeLayerRow(reportsCheck_, QStringLiteral("Storm reports"), QStringLiteral("reportsCheck"),
                 QStringLiteral("Tornado, hail, wind, rolling 24 h"), reportsBadge_,
                 QStringLiteral("reportsBadge"), true, {});
    makeLayerRow(lightningCheck_, QStringLiteral("GLM lightning jumps"),
                 QStringLiteral("lightningCheck"), QStringLiteral("Live GOES-18/19 jump cells"),
                 lightningBadge_, QStringLiteral("lightningBadge"), true,
                 QStringLiteral("Live GOES-18/19 total-lightning jump cells"));
    makeLayerRow(lightningFusionCheck_, QStringLiteral("Radar fusion"),
                 QStringLiteral("lightningFusionCheck"),
                 QStringLiteral("Jumps matched to reflectivity objects"), fusionBadge_,
                 QStringLiteral("fusionBadge"), true,
                 QStringLiteral("Associate GLM jumps with reflectivity objects and azimuthal shear"));
    makeLayerRow(placefilesCheck_, QStringLiteral("GR placefiles"), QStringLiteral("placefilesCheck"),
                 QStringLiteral("Chaser and spotter feeds"), placefilesBadge_,
                 QStringLiteral("placefilesBadge"), true,
                 QStringLiteral("Show loaded GR-compatible placefile markers and boundaries"));
    auto* placefileRow = new QHBoxLayout();
    placefileRow->setSpacing(4);
    addPlacefileButton_ = new QToolButton(controlBody);
    addPlacefileButton_->setObjectName(QStringLiteral("addPlacefileButton"));
    addPlacefileButton_->setText(QStringLiteral("File…"));
    addPlacefileButton_->setToolTip(QStringLiteral("Open a local GR placefile"));
    placefileRow->addWidget(addPlacefileButton_);
    addPlacefileUrlButton_ = new QToolButton(controlBody);
    addPlacefileUrlButton_->setObjectName(QStringLiteral("addPlacefileUrlButton"));
    addPlacefileUrlButton_->setText(QStringLiteral("URL…"));
    addPlacefileUrlButton_->setToolTip(QStringLiteral("Fetch a GR placefile URL with automatic refresh"));
    placefileRow->addWidget(addPlacefileUrlButton_);
    clearPlacefilesButton_ = new QToolButton(controlBody);
    clearPlacefilesButton_->setObjectName(QStringLiteral("clearPlacefilesButton"));
    clearPlacefilesButton_->setText(QStringLiteral("Clear"));
    clearPlacefilesButton_->setEnabled(false);
    clearPlacefilesButton_->setToolTip(QStringLiteral("Remove all loaded placefiles"));
    placefileRow->addWidget(clearPlacefilesButton_);
    layersGroup->addLayout(placefileRow);

    proximityFilterCheck_ = new QCheckBox(QStringLiteral("Filter warnings to selected radar / chase point"), controlBody);
    proximityFilterCheck_->setObjectName(QStringLiteral("proximityFilterCheck"));
    proximityFilterCheck_->setToolTip(QStringLiteral("Hide warnings outside the radius below, on screen and in alerts"));
    // Added to the layout later, inside the notify block below.
    auto* proximityRow = new QHBoxLayout();
    auto* proximityLabel = new QLabel(QStringLiteral("Proximity radius"), controlBody);
    proximityFilterRadius_ = new QDoubleSpinBox(controlBody);
    proximityFilterRadius_->setRange(10.0, 1500.0);
    proximityFilterRadius_->setSuffix(QStringLiteral(" km"));
    proximityFilterRadius_->setSingleStep(10.0);
    proximityFilterRadius_->setValue(200.0);
    proximityFilterRadius_->setEnabled(false);
    proximityRow->addWidget(proximityLabel);
    proximityRow->addWidget(proximityFilterRadius_, 1);
    // Added to the layout later, inside the notify block below.
    connect(proximityFilterCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        proximityFilterRadius_->setEnabled(enabled);
        pushProximityFilterToCanvases();
    });
    connect(proximityFilterRadius_, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double) { pushProximityFilterToCanvases(); });

    // Per-panel overlay overrides. The global layer rows above remain the
    // default for every panel. This box only appears when the scope selector
    // is set to "Active panel only"; editing here freezes the active panel's
    // visibility state until "Use global" clears it.
    perPanelBox_ = new QWidget(controlBody);
    perPanelBox_->setObjectName(QStringLiteral("perPanelBox"));
    auto* perPanelLayout = new QVBoxLayout(perPanelBox_);
    perPanelLayout->setContentsMargins(0, 0, 0, 0);
    perPanelLayout->setSpacing(4);
    layersGroup->addWidget(perPanelBox_);
    activePanelOverlayLabel_ = new QLabel(QStringLiteral("(no panel)"), controlBody);
    activePanelOverlayLabel_->setObjectName(QStringLiteral("activePanelOverlayLabel"));
    activePanelOverlayLabel_->setToolTip(
        QStringLiteral("Click a panel first; these controls then act on the active panel only."));
    perPanelLayout->addWidget(activePanelOverlayLabel_);
    activePanelWarnings_ = new QCheckBox(QStringLiteral("Warnings"), controlBody);
    activePanelReports_ = new QCheckBox(QStringLiteral("Storm reports"), controlBody);
    activePanelLightning_ = new QCheckBox(QStringLiteral("GLM lightning"), controlBody);
    activePanelFusion_ = new QCheckBox(QStringLiteral("Radar fusion"), controlBody);
    perPanelLayout->addWidget(activePanelWarnings_);
    perPanelLayout->addWidget(activePanelReports_);
    perPanelLayout->addWidget(activePanelLightning_);
    perPanelLayout->addWidget(activePanelFusion_);
    auto* perPanelRow = new QHBoxLayout();
    useGlobalButton_ = new QToolButton(controlBody);
    useGlobalButton_->setText(QStringLiteral("Use global"));
    useGlobalButton_->setToolTip(QStringLiteral("Drop the per-panel override and follow the global toggles above"));
    perPanelRow->addWidget(useGlobalButton_);
    perPanelRow->addStretch(1);
    perPanelLayout->addLayout(perPanelRow);
    const auto applyActivePanelOverrides = [this]() {
        if (!map_) return;
        RadarMapCanvas::OverlayVisibility overrides;
        overrides.warnings = activePanelWarnings_->isChecked();
        overrides.stormReports = activePanelReports_->isChecked();
        overrides.lightning = activePanelLightning_->isChecked();
        overrides.lightningFusion = activePanelFusion_->isChecked();
        map_->setOverlayOverrides(overrides);
        if (useGlobalButton_) useGlobalButton_->setEnabled(true);
    };
    connect(activePanelWarnings_, &QCheckBox::toggled, this, applyActivePanelOverrides);
    connect(activePanelReports_, &QCheckBox::toggled, this, applyActivePanelOverrides);
    connect(activePanelLightning_, &QCheckBox::toggled, this, applyActivePanelOverrides);
    connect(activePanelFusion_, &QCheckBox::toggled, this, applyActivePanelOverrides);
    connect(useGlobalButton_, &QToolButton::clicked, this, [this]() {
        if (!map_) return;
        map_->setOverlayOverrides(std::nullopt);
        refreshActivePanelOverlayUi();
    });
    refreshActivePanelOverlayUi();
    if (!cappedweather::glmL2ReaderAvailable()) {
        lightningCheck_->setEnabled(false);
        lightningCheck_->setChecked(false);
        lightningCheck_->setToolTip(QStringLiteral(
            "GLM lightning jumps require a CappedWeather build with netCDF-C support. "
            "Reinstall with netCDF or set CAPPEDWEATHER_REQUIRE_NETCDF=OFF to use a stub."));
        lightningFusionCheck_->setEnabled(false);
        lightningFusionCheck_->setChecked(false);
        lightningFusionCheck_->setToolTip(QStringLiteral(
            "Radar fusion requires live GLM data, which is disabled in this build."));
    }
    glmStatusNote_ = new QLabel(controlBody);
    glmStatusNote_->setObjectName(QStringLiteral("glmStatusNote"));
    glmStatusNote_->setWordWrap(true);
    glmStatusNote_->setVisible(false);
    controlLayout->addWidget(glmStatusNote_);
    warningFilterCombo_ = new QComboBox(controlBody);
    warningFilterCombo_->addItems({QStringLiteral("All warnings"), QStringLiteral("Tornado"),
                                   QStringLiteral("Severe Thunderstorm"), QStringLiteral("Flash Flood"),
                                   QStringLiteral("Extreme")});
    warningFilterCombo_->setToolTip(QStringLiteral("Filter visible warning polygons"));
    layersGroup->addWidget(warningFilterCombo_);
    connect(warningFilterCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        for (auto* panel : maps_)
            panel->setWarningFilter(index == 0 ? QString{} : warningFilterCombo_->itemText(index));
    });
    auto* reportKindRow = new QHBoxLayout();
    reportKindRow->setSpacing(8);
    tornadoCheck_ = new QCheckBox(QStringLiteral("Tornado"), controlBody);
    hailCheck_ = new QCheckBox(QStringLiteral("Hail"), controlBody);
    windCheck_ = new QCheckBox(QStringLiteral("Wind"), controlBody);
    for (auto* check : {tornadoCheck_, hailCheck_, windCheck_}) check->setChecked(true);
    reportKindRow->addWidget(tornadoCheck_);
    reportKindRow->addWidget(hailCheck_);
    reportKindRow->addWidget(windCheck_);
    reportKindRow->addStretch(1);
    layersGroup->addLayout(reportKindRow);
    connect(tornadoCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setStormReportKindVisible(StormReportKind::Tornado, visible);
    });
    connect(hailCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setStormReportKindVisible(StormReportKind::Hail, visible);
    });
    connect(windCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setStormReportKindVisible(StormReportKind::Wind, visible);
    });
    auto* opacityRow = new QHBoxLayout();
    opacityRow->setSpacing(8);
    auto* opacityLabel = new QLabel(QStringLiteral("Echo opacity"), controlBody);
    opacityLabel->setObjectName(QStringLiteral("section"));
    opacityRow->addWidget(opacityLabel);
    radarOpacitySlider_ = new QSlider(Qt::Horizontal, controlBody);
    radarOpacitySlider_->setRange(20, 100);
    radarOpacitySlider_->setValue(80);
    radarOpacitySlider_->setToolTip(QStringLiteral("Radar opacity"));
    opacityRow->addWidget(radarOpacitySlider_, 1);
    layersGroup->addLayout(opacityRow);
    connect(radarOpacitySlider_, &QSlider::valueChanged, this, [this](int opacity) {
        for (auto* panel : maps_) panel->setRadarOpacity(opacity);
    });
    auto* paletteRow = new QHBoxLayout();
    paletteRow->setSpacing(4);
    paletteCombo_ = new QComboBox(controlBody);
    paletteCombo_->addItems({QStringLiteral("Capped palette"), QStringLiteral("Native operational palette"),
                             QStringLiteral("Color-safe palette"), QStringLiteral("Custom palette")});
    paletteRow->addWidget(paletteCombo_, 1);
    connect(paletteCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        for (auto* panel : maps_) panel->setPalette(static_cast<RadarMapCanvas::PaletteKind>(index));
    });
    auto* paletteButton = new QToolButton(controlBody);
    paletteButton->setText(QStringLiteral("Palette..."));
    paletteButton->setToolTip(QStringLiteral("Import a value,color palette text file"));
    connect(paletteButton, &QToolButton::clicked, this, &MainWindow::importCustomPalette);
    paletteRow->addWidget(paletteButton);
    layersGroup->addLayout(paletteRow);
    belowThresholdCheck_ = new QCheckBox(QStringLiteral("Below threshold"), controlBody);
    belowThresholdCheck_->setObjectName(QStringLiteral("belowThresholdCheck"));
    layersGroup->addWidget(belowThresholdCheck_);
    connect(belowThresholdCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setBelowThresholdVisible(visible);
    });
    controlLayout->addSpacing(10);

    // Alert tuning lives next to the layers it governs, not in Settings.
    auto* notifyCaption = new QLabel(QStringLiteral("Notify me about new items"), controlBody);
    notifyCaption->setObjectName(QStringLiteral("groupCaption"));
    layersGroup->addWidget(notifyCaption);
    layersGroup->addWidget(proximityFilterCheck_);
    layersGroup->addLayout(proximityRow);
    auto* severityRow = new QHBoxLayout();
    auto* severityLabel = new QLabel(QStringLiteral("Minimum severity"), controlBody);
    severityCombo_ = new QComboBox(controlBody);
    severityCombo_->setObjectName(QStringLiteral("severityCombo"));
    severityCombo_->addItem(QStringLiteral("Minor (advisories + above)"), 0);
    severityCombo_->addItem(QStringLiteral("Moderate (watches + above)"), 1);
    severityCombo_->addItem(QStringLiteral("Severe (warnings only, default)"), 2);
    severityCombo_->addItem(QStringLiteral("Extreme (tornado / extreme wind only)"), 3);
    severityCombo_->setToolTip(QStringLiteral("Warnings below this NWS severity never notify"));
    severityRow->addWidget(severityLabel);
    severityRow->addWidget(severityCombo_, 1);
    layersGroup->addLayout(severityRow);
    auto* ageRow = new QHBoxLayout();
    auto* ageLabel = new QLabel(QStringLiteral("Only if issued within"), controlBody);
    ageLabel->setToolTip(QStringLiteral("Older warnings are logged silently"));
    ageSpin_ = new QSpinBox(controlBody);
    ageSpin_->setObjectName(QStringLiteral("ageSpin"));
    ageSpin_->setRange(0, 1440);
    ageSpin_->setSuffix(QStringLiteral(" min"));
    ageSpin_->setSingleStep(5);
    ageSpin_->setSpecialValueText(QStringLiteral("any age"));
    ageRow->addWidget(ageLabel);
    ageRow->addWidget(ageSpin_, 1);
    layersGroup->addLayout(ageRow);
    soundCheck_ = new QCheckBox(QStringLiteral("Sound"), controlBody);
    soundCheck_->setObjectName(QStringLiteral("soundCheck"));
    layersGroup->addWidget(soundCheck_);
    trayCheck_ = new QCheckBox(QStringLiteral("Tray notification"), controlBody);
    trayCheck_->setObjectName(QStringLiteral("trayCheck"));
    layersGroup->addWidget(trayCheck_);
    connect(severityCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { pushAlertDockToCenter(); });
    connect(ageSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { pushAlertDockToCenter(); });
    connect(soundCheck_, &QCheckBox::toggled, this, [this] { pushAlertDockToCenter(); });
    connect(trayCheck_, &QCheckBox::toggled, this, [this] { pushAlertDockToCenter(); });

    velocityModeCombo_ = new QComboBox(controlBody);
    velocityModeCombo_->setObjectName(QStringLiteral("velocityModeCombo"));
    velocityModeCombo_->addItems({QStringLiteral("Native velocity"), QStringLiteral("Dealiased velocity"),
                                  QStringLiteral("Storm-relative velocity")});
    velocityModeCombo_->setToolTip(QStringLiteral("Velocity processing for the active panel"));
    velocityBoxLayout->addWidget(velocityModeCombo_);
    nyquistSpin_ = new QDoubleSpinBox(controlBody);
    nyquistSpin_->setRange(1.0, 80.0);
    nyquistSpin_->setValue(25.0);
    nyquistSpin_->setSuffix(QStringLiteral(" m/s Nyq"));
    nyquistSpin_->setDecimals(1);
    nyquistSpin_->setToolTip(QStringLiteral("Nyquist velocity used by optional dealiasing"));
    velocityBoxLayout->addWidget(nyquistSpin_);
    stormDirectionSpin_ = new QDoubleSpinBox(controlBody);
    stormDirectionSpin_->setRange(0.0, 359.9);
    stormDirectionSpin_->setSuffix(QStringLiteral(" deg motion"));
    stormDirectionSpin_->setDecimals(1);
    stormDirectionSpin_->setToolTip(QStringLiteral("Direction toward which the storm is moving"));
    velocityBoxLayout->addWidget(stormDirectionSpin_);
    stormSpeedSpin_ = new QDoubleSpinBox(controlBody);
    stormSpeedSpin_->setRange(0.0, 80.0);
    stormSpeedSpin_->setSuffix(QStringLiteral(" m/s storm"));
    stormSpeedSpin_->setDecimals(1);
    velocityBoxLayout->addWidget(stormSpeedSpin_);
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

    auto* viewGroup = makeGroup(QStringLiteral("View"), QStringLiteral("view"));
    panelLayoutCombo_ = new QComboBox(controlBody);
    viewGroup->addWidget(panelLayoutCombo_);
    panelLayoutCombo_->setObjectName(QStringLiteral("panelLayoutCombo"));
    panelLayoutCombo_->addItems({QStringLiteral("1x1"), QStringLiteral("1x2"),
                                 QStringLiteral("2x1"), QStringLiteral("2x2")});
    panelLayoutCombo_->setToolTip(QStringLiteral("Radar panel layout"));
    viewGroup->addWidget(panelLayoutCombo_);
    connect(panelLayoutCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::setPanelLayout);
    auto* presetRow = new QHBoxLayout();
    presetsCombo_ = new QComboBox(controlBody);
    presetsCombo_->setObjectName(QStringLiteral("presetsCombo"));
    presetsCombo_->setToolTip(QStringLiteral("Named panel layout preset (built-in + user-saved)"));
    savePresetButton_ = new QToolButton(controlBody);
    savePresetButton_->setObjectName(QStringLiteral("savePresetButton"));
    savePresetButton_->setText(QStringLiteral("Save…"));
    savePresetButton_->setToolTip(QStringLiteral("Capture the current layout + per-panel products as a new preset"));
    presetRow->addWidget(presetsCombo_, 1);
    presetRow->addWidget(savePresetButton_);
    viewGroup->addLayout(presetRow);
    connect(presetsCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](const int index) {
                if (index < 0) return;
                const auto name = presetsCombo_->itemData(index).toString();
                if (name.isEmpty()) return;
                applyLayoutPreset(name);
            });
    connect(savePresetButton_, &QToolButton::clicked, this, &MainWindow::saveCurrentAsPreset);
    rebuildPresetsCombo();
    auto* panelHint = new QLabel(
        QStringLiteral("Click a panel to work on it. Product, tilt, and tools follow."), controlBody);
    panelHint->setObjectName(QStringLiteral("groupCaption"));
    panelHint->setWordWrap(true);
    viewGroup->addWidget(panelHint);

    auto* navigationGrid = new QGridLayout();
    navigationGrid->setSpacing(4);
    int navigationPosition = 0;
    const auto addViewButton = [controlBody, navigationGrid,
                                &navigationPosition](const QString& text, const QString& tip) {
        auto* button = new QToolButton(controlBody);
        button->setText(text);
        button->setToolTip(tip);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        navigationGrid->addWidget(button, navigationPosition / 3, navigationPosition % 3);
        ++navigationPosition;
        return button;
    };
    auto* nationalButton = addViewButton(QStringLiteral("Home"), QStringLiteral("National view"));
    auto* recenterButton = addViewButton(QStringLiteral("Recenter"), QStringLiteral("Center selected radar (R)"));
    auto* zoomOutButton = addViewButton(QStringLiteral("−"), QStringLiteral("Zoom out (-)"));
    auto* zoomInButton = addViewButton(QStringLiteral("+"), QStringLiteral("Zoom in (+)"));
    connect(nationalButton, &QToolButton::clicked, this, [this] { map_->showNationalView(); });
    connect(recenterButton, &QToolButton::clicked, this, [this] { map_->recenterSelected(); });
    connect(zoomOutButton, &QToolButton::clicked, this, [this] { map_->zoomOut(); });
    connect(zoomInButton, &QToolButton::clicked, this, [this] { map_->zoomIn(); });
    auto* settingsButton = addViewButton(QStringLiteral("Settings"),
                                         QStringLiteral("Operational and chase settings"));
    connect(settingsButton, &QToolButton::clicked, this, &MainWindow::showSettingsDialog);
    viewGroup->addLayout(navigationGrid);

    auto* toolsGroup = makeGroup(QStringLiteral("Tools"), QStringLiteral("tools"));
    auto* toolsGrid = new QGridLayout();
    toolsGrid->setSpacing(4);
    int toolsPosition = 0;
    const auto addToolButton = [controlBody, toolsGrid,
                                &toolsPosition](const QString& text, const QString& tip) {
        auto* button = new QToolButton(controlBody);
        button->setText(text);
        button->setToolTip(tip);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        toolsGrid->addWidget(button, toolsPosition / 2, toolsPosition % 2);
        ++toolsPosition;
        return button;
    };
    measureButton_ = addToolButton(QStringLiteral("Measure"), QStringLiteral("Measure distance and bearing"));
    measureButton_->setCheckable(true);
    connect(measureButton_, &QToolButton::toggled, this, [this](bool enabled) {
        if (enabled && crossSectionButton_) crossSectionButton_->setChecked(false);
        map_->setMeasureMode(enabled);
    });
    crossSectionButton_ = addToolButton(QStringLiteral("Cross-section"),
                                       QStringLiteral("Draw a vertical cross-section through all tilts"));
    crossSectionButton_->setObjectName(QStringLiteral("crossSectionButton"));
    crossSectionButton_->setCheckable(true);
    connect(crossSectionButton_, &QToolButton::toggled, this, [this](bool enabled) {
        if (enabled) measureButton_->setChecked(false);
        map_->setCrossSectionMode(enabled);
    });
    toolsGroup->addLayout(toolsGrid);
    controlLayout->addStretch(1);
    controlScroll->setWidget(controlBody);
    controlDock_->setWidget(controlScroll);
    addDockWidget(Qt::LeftDockWidgetArea, controlDock_);
    // Two-column chip/layer rows need ~320 px; without this the dock opens
    // at the bare minimum and clips them.
    resizeDocks({controlDock_}, {324}, Qt::Horizontal);

    playbackDock_ = new QDockWidget(QStringLiteral("PLAYBACK"), this);
    playbackDock_->setObjectName(QStringLiteral("playbackDock"));
    playbackDock_->setFeatures(QDockWidget::DockWidgetMovable);
    auto* playbackBody = new QWidget(playbackDock_);
    playbackBody->setObjectName(QStringLiteral("dockBody"));
    auto* playbackLayout = new QVBoxLayout(playbackBody);
    playbackLayout->setContentsMargins(8, 6, 8, 7);
    playbackLayout->setSpacing(4);
    auto* transportRow = new QHBoxLayout();
    transportRow->setContentsMargins(0, 0, 0, 0);
    transportRow->setSpacing(4);
    previousButton_ = new QToolButton(playbackBody);
    previousButton_->setObjectName(QStringLiteral("previousFrameButton"));
    previousButton_->setText(QStringLiteral("Previous"));
    transportRow->addWidget(previousButton_);
    playButton_ = new QToolButton(playbackBody);
    playButton_->setObjectName(QStringLiteral("playButton"));
    playButton_->setText(QStringLiteral("Play"));
    playButton_->setCheckable(true);
    transportRow->addWidget(playButton_);
    nextButton_ = new QToolButton(playbackBody);
    nextButton_->setObjectName(QStringLiteral("nextFrameButton"));
    nextButton_->setText(QStringLiteral("Next"));
    transportRow->addWidget(nextButton_);
    liveButton_ = new QToolButton(playbackBody);
    liveButton_->setObjectName(QStringLiteral("liveButton"));
    liveButton_->setText(QStringLiteral("LIVE"));
    liveButton_->setCheckable(true);
    liveButton_->setChecked(true);
    liveButton_->setToolTip(QStringLiteral("Follow the newest complete volume (L)"));
    transportRow->addWidget(liveButton_);
    frameSlider_ = new QSlider(Qt::Horizontal, playbackBody);
    frameSlider_->setObjectName(QStringLiteral("frameSlider"));
    frameSlider_->setRange(0, 0);
    frameSlider_->installEventFilter(this);
    transportRow->addWidget(frameSlider_, 1);
    speedCombo_ = new QComboBox(playbackBody);
    speedCombo_->setObjectName(QStringLiteral("speedCombo"));
    speedCombo_->addItem(QStringLiteral("0.5x"), 2000);
    speedCombo_->addItem(QStringLiteral("1x"), 1000);
    speedCombo_->addItem(QStringLiteral("2x"), 500);
    speedCombo_->addItem(QStringLiteral("4x"), 250);
    speedCombo_->addItem(QStringLiteral("8x"), 125);
    speedCombo_->addItem(QStringLiteral("16x"), 62);
    speedCombo_->setCurrentIndex(2);
    transportRow->addWidget(speedCombo_);
    timeJumpEdit_ = new QDateTimeEdit(playbackBody);
    timeJumpEdit_->setObjectName(QStringLiteral("timeJumpEdit"));
    timeJumpEdit_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    timeJumpEdit_->setCalendarPopup(true);
    timeJumpEdit_->setToolTip(QStringLiteral("Jump to the closest loaded frame at this time (UTC)"));
    timeJumpEdit_->setSpecialValueText(QStringLiteral("(jump to time)"));
    timeJumpEdit_->setMinimumDateTime(QDateTime(QDate(2000, 1, 1), QTime(0, 0)));
    timeJumpEdit_->setDateTime(QDateTime(QDate(2000, 1, 1), QTime(0, 0)));
    timeJumpEdit_->setMaximumDateTime(QDateTime::currentDateTimeUtc().addYears(1));
    transportRow->addWidget(timeJumpEdit_);
    playbackLayout->addLayout(transportRow);

    auto* detailsRow = new QHBoxLayout();
    detailsRow->setContentsMargins(0, 0, 0, 0);
    detailsRow->setSpacing(4);
    frameLabel_ = new QLabel(QStringLiteral("No volume history"), playbackBody);
    frameLabel_->setObjectName(QStringLiteral("frameLabel"));
    frameLabel_->setMinimumWidth(0);
    frameLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    frameLabel_->setFont(monoFont);
    detailsRow->addWidget(frameLabel_, 1);
    auto* snapshotButton = new QToolButton(playbackBody);
    snapshotButton->setObjectName(QStringLiteral("snapshotButton"));
    snapshotButton->setText(QStringLiteral("Snapshot"));
    connect(snapshotButton, &QToolButton::clicked, this, &MainWindow::exportSnapshot);
    detailsRow->addWidget(snapshotButton);
    auto* timelineButton = new QToolButton(playbackBody);
    timelineButton->setObjectName(QStringLiteral("timelineButton"));
    timelineButton->setText(QStringLiteral("Timeline GIF"));
    timelineButton->setToolTip(QStringLiteral("Export loaded volume frames as an animated GIF"));
    connect(timelineButton, &QToolButton::clicked, this, &MainWindow::exportTimelineAnimation);
    detailsRow->addWidget(timelineButton);
    playbackLayout->addLayout(detailsRow);
    auto* archiveRow = new QHBoxLayout();
    archiveRow->setContentsMargins(0, 0, 0, 0);
    archiveRow->setSpacing(4);
    auto* archiveLabel = new QLabel(QStringLiteral("OFFLINE / ARCHIVE"), playbackBody);
    archiveLabel->setObjectName(QStringLiteral("section"));
    archiveRow->addWidget(archiveLabel);
    archiveRow->addStretch(1);
    openArchiveFilesButton_ = new QToolButton(playbackBody);
    openArchiveFilesButton_->setObjectName(QStringLiteral("openArchiveFilesButton"));
    openArchiveFilesButton_->setText(QStringLiteral("L2 files…"));
    openArchiveFilesButton_->setToolTip(QStringLiteral("Load local NOAA Archive-II Level II files for the selected radar"));
    connect(openArchiveFilesButton_, &QToolButton::clicked, this, &MainWindow::openArchiveFiles);
    archiveRow->addWidget(openArchiveFilesButton_);
    openArchiveFolderButton_ = new QToolButton(playbackBody);
    openArchiveFolderButton_->setObjectName(QStringLiteral("openArchiveFolderButton"));
    openArchiveFolderButton_->setText(QStringLiteral("L2 folder…"));
    openArchiveFolderButton_->setToolTip(QStringLiteral("Load all Archive-II Level II files in a local folder"));
    connect(openArchiveFolderButton_, &QToolButton::clicked, this, &MainWindow::openArchiveFolder);
    archiveRow->addWidget(openArchiveFolderButton_);
    awsArchiveButton_ = new QToolButton(playbackBody);
    awsArchiveButton_->setObjectName(QStringLiteral("awsArchiveButton"));
    awsArchiveButton_->setText(QStringLiteral("AWS archive…"));
    awsArchiveButton_->setToolTip(QStringLiteral("Load historical Level II volumes from the public NOAA/Unidata AWS archive"));
    connect(awsArchiveButton_, &QToolButton::clicked, this, &MainWindow::openAwsArchive);
    archiveRow->addWidget(awsArchiveButton_);
    playbackLayout->addLayout(archiveRow);
    overlayStatusLabel_ = new QLabel(playbackBody);
    overlayStatusLabel_->setObjectName(QStringLiteral("section"));
    overlayStatusLabel_->hide();
    playbackDock_->setWidget(playbackBody);
    addDockWidget(Qt::BottomDockWidgetArea, playbackDock_);
    resizeDocks({controlDock_}, {272}, Qt::Horizontal);

    auto* status = statusBar();
    unifiedStatusLabel_ = new QLabel(QStringLiteral("Waiting for live services"));
    unifiedStatusLabel_->setObjectName(QStringLiteral("unifiedStatusLabel"));
    unifiedStatusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    unifiedStatusLabel_->setMinimumWidth(0);
    unifiedStatusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    unifiedStatusLabel_->setFont(monoFont);
    status->addWidget(unifiedStatusLabel_, 1);
    loadingProgress_ = new QProgressBar();
    loadingProgress_->setTextVisible(false);
    loadingProgress_->setFixedWidth(130);
    loadingProgress_->hide();
    status->addPermanentWidget(loadingProgress_);
    retryButton_ = new QToolButton();
    retryButton_->setText(QStringLiteral("Retry"));
    retryButton_->hide();
    status->addPermanentWidget(retryButton_);
    cancelButton_ = new QToolButton();
    cancelButton_->setText(QStringLiteral("Cancel"));
    cancelButton_->hide();
    status->addPermanentWidget(cancelButton_);

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
    const auto summary = QStringLiteral("%1 · %2 · %3 m")
                             .arg(QString::fromStdString(site.name),
                                  QString::fromStdString(site.owner))
                             .arg(site.antennaElevationMeters, 0, 'f', 0);
    stationSummaryLabel_->setText(summary);
    stationSummaryLabel_->setToolTip(summary);
    const QSignalBlocker blocker(favoriteButton_);
    const auto favorite = favoriteRadarIds_.contains(selectedRadarId_);
    favoriteButton_->setChecked(favorite);
    favoriteButton_->setText(favorite ? QStringLiteral("★") : QStringLiteral("☆"));
}

void MainWindow::updateUnifiedStatus() {
    if (!unifiedStatusLabel_) return;
    const auto status = QStringLiteral("Radar: %1    |    NWS/SPC: %2    |    GLM: %3")
                            .arg(radarStatus_, overlayStatus_, lightningStatus_);
    unifiedStatusLabel_->setText(status);
    unifiedStatusLabel_->setToolTip(status);
    if (glmStatusNote_) {
        if (!cappedweather::glmL2ReaderAvailable()) {
            glmStatusNote_->setText(QStringLiteral("GLM disabled (netCDF-C unavailable)"));
            glmStatusNote_->setStyleSheet(QStringLiteral("color: #ff8a80;"));
            glmStatusNote_->setVisible(true);
        } else if (lightningStatus_.startsWith(QStringLiteral("GLM disabled"))) {
            glmStatusNote_->setText(lightningStatus_);
            glmStatusNote_->setStyleSheet(QStringLiteral("color: #ff8a80;"));
            glmStatusNote_->setVisible(true);
        } else {
            glmStatusNote_->setVisible(false);
            glmStatusNote_->setStyleSheet(QString());
        }
    }
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

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (emptyStateHint_ && emptyStateHint_->isVisible()) {
        const auto* parent = emptyStateHint_->parentWidget();
        if (parent) {
            const auto hintWidth = std::min(520, parent->width() - 40);
            emptyStateHint_->setGeometry(
                (parent->width() - hintWidth) / 2,
                (parent->height() - emptyStateHint_->sizeHint().height()) / 2,
                hintWidth,
                emptyStateHint_->sizeHint().height());
        }
    }
}

void MainWindow::radarSelected(const QModelIndex& index) {
    if (emptyStateHint_) emptyStateHint_->hide();
    if (!index.isValid()) {
        return;
    }
    const auto radarId = index.data(RadarListModel::RadarIdRole).toString();
    if (radarId == selectedRadarId_ &&
        std::all_of(maps_.cbegin(), maps_.cend(), [&radarId](const RadarMapCanvas* panel) {
            return panel->radarId() == radarId;
        })) {
        map_->recenterSelected();
        return;
    }
    selectedRadarId_ = radarId;
    while (panelRadarIds_.size() < maps_.size()) panelRadarIds_.push_back({});
    for (auto& panelRadarId : panelRadarIds_) panelRadarId = radarId;
    const auto existed = radarSessions_.contains(radarId);
    const auto session = sessionFor(radarId);
    lastVolumeTime_ = {};
    setRadarHealth(RadarHealthState::Stale, QStringLiteral("Loading %1...").arg(radarId));
    recentRadarIds_.removeAll(radarId);
    recentRadarIds_.prepend(radarId);
    while (recentRadarIds_.size() > 8) recentRadarIds_.removeLast();
    QSettings settings;
    settings.setValue(QStringLiteral("recentRadars"), recentRadarIds_);
    settings.setValue(QStringLiteral("lastRadar"), radarId);
    for (int panel = 0; panel < maps_.size(); ++panel)
        settings.setValue(QStringLiteral("panels/radar%1").arg(panel), radarId);
    rebuildStationPicker();
    stopAnimation();
    session->followLatest = true;
    liveButton_->setChecked(true);
    updateProductAvailability();
    updatePlaybackControls();
    for (auto* panel : maps_) {
        panel->setSelectedRadar(radarId);
        panel->setLoading(session->history.empty(), QStringLiteral("Loading %1 radar data").arg(radarId));
        panel->setVolumeQuality(0, 0, 0);
    }
    updatePanelAttribution();
    const auto contextRow = model_->rowForRadar(radarId);
    if (contextRow >= 0) {
        const auto& site = model_->siteAt(contextRow);
        if (site.latitude && site.longitude) {
            placefileSource_->setContext(*site.latitude, *site.longitude);
            if (alertCenter_) {
                const GeographicCoordinate coord{*site.latitude, *site.longitude};
                alertCenter_->setSelectedRadar(coord);
            }
        }
    }
    pushProximityFilterToCanvases();
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
    if (!existed) clearDownloadedFor(radarId);
    session->requestActive = true;
    requestLatestFor(radarId);
    if (!session->history.empty()) {
        session->frameIndex = static_cast<int>(session->history.size()) - 1;
        showFrame(session->frameIndex);
    }
}

void MainWindow::requestSelectedRadar() {
    if (selectedRadarId_.isEmpty()) return;
    if (const auto session = sessionFor(selectedRadarId_)) session->requestActive = true;
    requestLatestFor(selectedRadarId_);
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
        setRadarHealth(RadarHealthState::Offline,
                       QStringLiteral("Last fetch failed: %1").arg(message));
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

    // The NWS feed carries no European sites; keep the OPERA catalog.
    for (const auto& site : RadarCatalog::operaSites()) sites.push_back(site);
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

void MainWindow::chunkDecoded(const QString& radarId, const QString& volumeId, const QString&,
                              DecodedChunk decoded, const QDateTime&) {
    const auto session = sessionFor(radarId, false);
    if (!session || volumeId != session->pendingVolumeId) return;
    acceptDecodedChunk(radarId, volumeId, std::move(decoded));
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
    if (session->derivingVolumeIds.contains(volumeId)) return;
    session->derivingVolumeIds.insert(volumeId);
    auto volume = std::make_shared<DecodedChunk>(std::move(session->pendingVolume));
    double radarElevationMeters = 0.0;
    const auto radarRow = model_->rowForRadar(radarId);
    if (radarRow >= 0) radarElevationMeters = model_->siteAt(radarRow).antennaElevationMeters;
    for (auto* panel : maps_) {
        if (panel->radarId() == radarId)
            panel->setLoading(true, QStringLiteral("%1 deriving ET18 + VIL").arg(radarId));
    }
    if (radarId == selectedRadarId()) {
        radarStatus_ = QStringLiteral("%1 deriving Echo Tops + VIL in background").arg(radarId);
        updateUnifiedStatus();
    }
    const QPointer<MainWindow> guard(this);
    QThreadPool::globalInstance()->start(
        [guard, volume, radarId, volumeId, chunkCount, failedChunks, receivedAt,
         radarElevationMeters] {
            appendDerivedProducts(*volume, radarElevationMeters);
            if (!guard) return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, volume, radarId, volumeId, chunkCount, failedChunks, receivedAt] {
                    if (guard)
                        guard->finalizeVolumeReady(radarId, volumeId, chunkCount, failedChunks,
                                                   receivedAt, volume);
                },
                Qt::QueuedConnection);
        });
}

void MainWindow::finalizeVolumeReady(const QString& radarId, const QString& volumeId,
                                     const int chunkCount, const int failedChunks,
                                     const QDateTime& receivedAt,
                                     std::shared_ptr<DecodedChunk> volume) {
    const auto session = sessionFor(radarId, false);
    if (!session || !session->derivingVolumeIds.remove(volumeId)) return;
    const auto isCurrentVolume = volumeId == session->pendingVolumeId;
    const auto followLatest = session->history.empty() || session->followLatest;
    const auto selectedVolume = session->frameIndex >= 0 &&
                                session->frameIndex < static_cast<int>(session->history.size())
                                    ? session->history.at(static_cast<std::size_t>(session->frameIndex)).volumeId
                                    : QString{};
    const auto duplicate = std::find_if(session->history.begin(), session->history.end(), [&volumeId](const RadarFrame& frame) {
        return frame.volumeId == volumeId;
    });
    if (duplicate == session->history.end()) {
        session->history.push_back({volumeId, std::move(volume), receivedAt, chunkCount, failedChunks});
    }
    std::sort(session->history.begin(), session->history.end(), [](const RadarFrame& left, const RadarFrame& right) {
        return left.dataTime == right.dataTime ? left.volumeId < right.volumeId
                                               : left.dataTime < right.dataTime;
    });
    const auto maximumHistory = static_cast<std::size_t>(std::max(2, maximumHistoryFrames_));
    if (session->history.size() > maximumHistory) {
        session->history.erase(session->history.begin(), session->history.begin() +
            static_cast<std::ptrdiff_t>(session->history.size() - maximumHistory));
    }
    if (isCurrentVolume) session->pendingVolumeId.clear();
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
        if (isCurrentVolume) {
            loadingProgress_->hide();
            cancelButton_->hide();
            retryButton_->hide();
        }
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
    if (!session->requestActive) {
        radarStatus_ = QStringLiteral("%1 · %2 real volume%3 · %4")
                           .arg(radarId).arg(session->history.size())
                           .arg(session->history.size() == 1 ? QString{} : QStringLiteral("s"))
                           .arg(failedChunks == 0 ? QStringLiteral("complete")
                                                  : QStringLiteral("partial: %1 chunk%2 unavailable")
                                                        .arg(failedChunks)
                                                        .arg(failedChunks == 1 ? QString{} : QStringLiteral("s")));
    }
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
            requestRecentFor(radarId, historyVolumeCount_);
        });
    }
    if (radarId == selectedRadarId() && session->playWhenHistoryReady && session->history.size() >= 2) {
        session->playWhenHistoryReady = false;
        QTimer::singleShot(0, this, [this] { playButton_->setChecked(true); });
    }
    if (radarId == selectedRadarId_) {
        if (!lastVolumeTime_.isValid() || receivedAt > lastVolumeTime_) lastVolumeTime_ = receivedAt;
        recomputeRadarHealth();
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
    requestRecentFor(selectedRadarId_, historyVolumeCount_);
}

void MainWindow::rebuildTiltCombo() {
    struct TiltSummary {
        int number = 0;
        double angleTotal = 0.0;
        int rayCount = 0;
    };
    const auto previousElevation = map_->elevationFilter().value_or(-1);
    if (isDerivedProduct(map_->product())) {
        const QSignalBlocker blocker(tiltCombo_);
        tiltCombo_->clear();
        tiltCombo_->addItem(QStringLiteral("All tilts (volume derived)"));
        tiltCombo_->setEnabled(false);
        map_->setElevationFilter(std::nullopt);
        rebuildTiltGrid();
        return;
    }
    std::vector<TiltSummary> tilts;
    const auto session = activeSession();
    if (!session || session->frameIndex < 0 ||
        session->frameIndex >= static_cast<int>(session->history.size())) {
        tiltCombo_->clear();
        tiltCombo_->addItem(QStringLiteral("Waiting for decoded tilts"));
        tiltCombo_->setEnabled(false);
        map_->setElevationFilter(std::nullopt);
        rebuildTiltGrid();
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
    rebuildTiltGrid();
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
    refreshLayerBadges();
    updatePanelAttribution();
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
    theme_ = appThemeFromInt(settings.value(QStringLiteral("ui/theme"), 1).toInt());
    applyTheme(theme_);
    if (alertCenter_) {
        AlertConfig alertConfig = alertCenter_->config();
        alertConfig.enabled = settings.value(QStringLiteral("alerts/enabled"), alertConfig.enabled).toBool();
        alertConfig.soundEnabled = settings.value(QStringLiteral("alerts/sound"), alertConfig.soundEnabled).toBool();
        alertConfig.systemNotificationEnabled = settings.value(QStringLiteral("alerts/notification"), alertConfig.systemNotificationEnabled).toBool();
        alertConfig.geographicFilterEnabled = settings.value(QStringLiteral("alerts/geoFilter"), alertConfig.geographicFilterEnabled).toBool();
        alertConfig.geographicFilterRadiusKm = std::clamp(
            settings.value(QStringLiteral("alerts/geoRadiusKm"), alertConfig.geographicFilterRadiusKm).toDouble(),
            10.0, 1500.0);
        alertConfig.eventFilterEnabled = settings.value(QStringLiteral("alerts/eventFilterEnabled"), alertConfig.eventFilterEnabled).toBool();
        alertConfig.eventFilter = settings.value(QStringLiteral("alerts/eventFilter"), alertConfig.eventFilter).toStringList();
        alertConfig.lightningSigmaThreshold = std::clamp(
            settings.value(QStringLiteral("alerts/lightningSigma"), alertConfig.lightningSigmaThreshold).toDouble(),
            1.0, 10.0);
        alertConfig.repeatSuppressionMinutes = std::clamp(
            settings.value(QStringLiteral("alerts/repeatSuppressionMinutes"), alertConfig.repeatSuppressionMinutes).toInt(),
            1, 240);
        alertConfig.minSeverity = std::clamp(
            settings.value(QStringLiteral("alerts/minSeverity"), alertConfig.minSeverity).toInt(),
            0, 3);
        alertConfig.maxWarningAgeMinutes = std::clamp(
            settings.value(QStringLiteral("alerts/maxWarningAgeMinutes"),
                           alertConfig.maxWarningAgeMinutes)
                .toInt(),
            0, 1440);
        alertConfig.customSoundPath = settings.value(QStringLiteral("alerts/customSoundPath"), alertConfig.customSoundPath).toString();
        alertCenter_->setConfig(alertConfig);
        alertCenter_->setChaseMode(chaseMode_, chaseLatitude_, chaseLongitude_);
        // The sidebar owns geo filtering now: seed its widgets from the
        // stored alert config so a previous dialog setting carries over.
        {
            const QSignalBlocker geoBlocker(proximityFilterCheck_);
            const QSignalBlocker radiusBlocker(proximityFilterRadius_);
            proximityFilterCheck_->setChecked(alertConfig.geographicFilterEnabled);
            proximityFilterRadius_->setValue(alertConfig.geographicFilterRadiusKm);
            proximityFilterRadius_->setEnabled(alertConfig.geographicFilterEnabled);
        }
    }
    for (int panel = 0; panel < maps_.size(); ++panel) {
        const auto defaultProduct = static_cast<int>(maps_.at(panel)->product());
        const auto product = std::clamp(settings.value(QStringLiteral("panels/product%1").arg(panel),
                                                        defaultProduct).toInt(), 0,
                                  static_cast<int>(kProductCount) - 1);
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
    }
    const auto sharedRadar = settings.value(
        QStringLiteral("lastRadar"), settings.value(QStringLiteral("panels/radar0"))).toString();
    for (auto& panelRadarId : panelRadarIds_) panelRadarId = sharedRadar;

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
    if (lightningCheck_->isEnabled())
        lightningCheck_->setChecked(settings.value(QStringLiteral("overlays/lightning"), true).toBool());
    if (lightningFusionCheck_->isEnabled())
        lightningFusionCheck_->setChecked(settings.value(QStringLiteral("overlays/lightningFusion"), true).toBool());
    placefilesCheck_->setChecked(settings.value(QStringLiteral("overlays/placefiles"), true).toBool());
    // Map legacy 3-item speed index (0=1x, 1=2x, 2=4x) onto the new 6-item
    // speed combo. After the P1 round 2 expansion, 1x is at index 1, 2x at 2,
    // 4x at 3, and we added 0.5x, 8x, 16x at 0, 4, 5.
    const auto rawSpeed = settings.value(QStringLiteral("timeline/speed"), 2).toInt();
    int migratedSpeed = rawSpeed;
    if (rawSpeed >= 0 && rawSpeed <= 2) migratedSpeed = rawSpeed + 1;
    speedCombo_->setCurrentIndex(std::clamp(migratedSpeed, 0, speedCombo_->count() - 1));
    panelLayoutCombo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("panels/layout"), 0).toInt(),
                                                  0, panelLayoutCombo_->count() - 1));
    scopeCombo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("ui/layerScope"), 0).toInt(),
                                            0, scopeCombo_->count() - 1));
    refreshAlertDockFromCenter();
    syncProductChips();
    rebuildTiltGrid();
    refreshScopeVisibility();
    activateMap(maps_.front());
}

void MainWindow::saveSettings() const {
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/dockState"), saveState(1));
    if (controlDock_ && controlDock_->isVisible())
        settings.setValue(QStringLiteral("window/controlDockWidth"), controlDock_->width());
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
    settings.setValue(QStringLiteral("overlays/lightning"), lightningCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/lightningFusion"), lightningFusionCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/placefiles"), placefilesCheck_->isChecked());
    settings.setValue(QStringLiteral("timeline/speed"), speedCombo_->currentIndex());
    settings.setValue(QStringLiteral("timeline/maximumFrames"), maximumHistoryFrames_);
    settings.setValue(QStringLiteral("timeline/historyVolumes"), historyVolumeCount_);
    settings.setValue(QStringLiteral("chase/enabled"), chaseMode_);
    settings.setValue(QStringLiteral("chase/latitude"), chaseLatitude_);
    settings.setValue(QStringLiteral("chase/longitude"), chaseLongitude_);
    settings.setValue(QStringLiteral("panels/layout"), panelLayoutCombo_->currentIndex());
        settings.setValue(QStringLiteral("panels/synchronizeNavigation"), synchronizePanels_);
        settings.setValue(QStringLiteral("ui/theme"), static_cast<int>(theme_));
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
    if (alertCenter_) {
        const auto alertConfig = alertCenter_->config();
        settings.setValue(QStringLiteral("alerts/enabled"), alertConfig.enabled);
        settings.setValue(QStringLiteral("alerts/sound"), alertConfig.soundEnabled);
        settings.setValue(QStringLiteral("alerts/notification"), alertConfig.systemNotificationEnabled);
        settings.setValue(QStringLiteral("alerts/geoFilter"), alertConfig.geographicFilterEnabled);
        settings.setValue(QStringLiteral("alerts/geoRadiusKm"), alertConfig.geographicFilterRadiusKm);
        settings.setValue(QStringLiteral("alerts/eventFilterEnabled"), alertConfig.eventFilterEnabled);
        settings.setValue(QStringLiteral("alerts/eventFilter"), alertConfig.eventFilter);
        settings.setValue(QStringLiteral("alerts/lightningSigma"), alertConfig.lightningSigmaThreshold);
        settings.setValue(QStringLiteral("alerts/repeatSuppressionMinutes"), alertConfig.repeatSuppressionMinutes);
        settings.setValue(QStringLiteral("alerts/minSeverity"), alertConfig.minSeverity);
        settings.setValue(QStringLiteral("alerts/maxWarningAgeMinutes"), alertConfig.maxWarningAgeMinutes);
        settings.setValue(QStringLiteral("alerts/customSoundPath"), alertConfig.customSoundPath);
    }
}

void MainWindow::showSettingsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("CappedWeather Settings"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();
    auto* themeCombo = new QComboBox(&dialog);
    themeCombo->addItem(appThemeName(AppTheme::Classic), static_cast<int>(AppTheme::Classic));
    themeCombo->addItem(appThemeName(AppTheme::Prototype), static_cast<int>(AppTheme::Prototype));
    themeCombo->setCurrentIndex(themeCombo->findData(static_cast<int>(theme_)));
    themeCombo->setToolTip(QStringLiteral("Switch the application identity live. Squall is the redesign prototype."));
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
    auto* detectButton = new QPushButton(QStringLiteral("Detect from IP"), &dialog);
    detectButton->setToolTip(QStringLiteral("Coarse: query a public IP geolocation service and populate the chase coords"));
    auto* chaseRow = new QHBoxLayout();
    chaseRow->addWidget(detectButton);
    chaseRow->addStretch(1);
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
    auto* lightningRefresh = new QSpinBox(&dialog);
    lightningRefresh->setRange(10, 300);
    lightningRefresh->setSuffix(QStringLiteral(" s"));
    lightningRefresh->setValue(QSettings().value(QStringLiteral("lightning/refreshSeconds"), 20).toInt());
    auto* lightningBackfill = new QSpinBox(&dialog);
    lightningBackfill->setRange(13, 60);
    lightningBackfill->setSuffix(QStringLiteral(" min"));
    lightningBackfill->setValue(QSettings().value(QStringLiteral("lightning/backfillMinutes"), 18).toInt());
    lightningBackfill->setToolTip(QStringLiteral("How much GLM history to pull on first start (minutes)"));
    auto* lightningCell = new QDoubleSpinBox(&dialog);
    lightningCell->setRange(0.05, 1.0);
    lightningCell->setSingleStep(0.05);
    lightningCell->setDecimals(2);
    lightningCell->setSuffix(QStringLiteral(" deg"));
    lightningCell->setValue(QSettings().value(QStringLiteral("lightning/cellSizeDeg"), 0.25).toDouble());
    lightningCell->setToolTip(QStringLiteral("Spatial binning size for the flash-rate grid (degrees lat/lon)"));
    auto* lightningRate = new QDoubleSpinBox(&dialog);
    lightningRate->setRange(1.0, 100.0);
    lightningRate->setSuffix(QStringLiteral(" flashes/min"));
    lightningRate->setValue(QSettings().value(QStringLiteral("lightning/minFlashRate"), 12.0).toDouble());
    lightningRate->setToolTip(QStringLiteral("Cells below this flash rate are not eligible for jump detection"));
    auto* lightningSigma = new QDoubleSpinBox(&dialog);
    lightningSigma->setRange(1.0, 10.0);
    lightningSigma->setSingleStep(0.25);
    lightningSigma->setSuffix(QStringLiteral(" sigma"));
    lightningSigma->setValue(QSettings().value(QStringLiteral("lightning/sigmaThreshold"), 2.0).toDouble());
    lightningSigma->setToolTip(QStringLiteral("Sigma threshold for a flash-rate change to count as a jump"));
    auto* lightningWindow = new QDoubleSpinBox(&dialog);
    lightningWindow->setRange(5.0, 30.0);
    lightningWindow->setSuffix(QStringLiteral(" min"));
    lightningWindow->setValue(QSettings().value(QStringLiteral("lightning/sigmaWindowMinutes"), 10.0).toDouble());
    lightningWindow->setToolTip(QStringLiteral("Window over which the flash-rate sigma is computed"));
    auto* memoryCache = new QSpinBox(&dialog);
    memoryCache->setRange(8, 256);
    memoryCache->setSuffix(QStringLiteral(" MB/panel"));
    memoryCache->setValue(QSettings().value(QStringLiteral("cache/memoryMbPerPanel"), 32).toInt());
    auto* diskCache = new QSpinBox(&dialog);
    diskCache->setRange(16, 1024);
    diskCache->setSuffix(QStringLiteral(" MB/panel"));
    diskCache->setValue(QSettings().value(QStringLiteral("cache/diskMbPerPanel"), 64).toInt());
    form->addRow(QStringLiteral("Theme"), themeCombo);
    form->addRow(QStringLiteral("Chase mode"), chaseCheck);
    form->addRow(QStringLiteral("Chase latitude"), latitude);
    form->addRow(QStringLiteral("Chase longitude"), longitude);
    form->addRow(QString(), chaseRow);
    connect(detectButton, &QPushButton::clicked, &dialog, [&dialog, latitude, longitude, detectButton]() {
        detectButton->setEnabled(false);
        detectButton->setText(QStringLiteral("Detecting…"));
        auto* nam = new QNetworkAccessManager(&dialog);
        auto* reply = nam->get(makeRequest(QUrl(QStringLiteral("https://ipwho.is/")),
                                           QStringLiteral("application/json"),
                                           QStringLiteral(" (chase-position detection)")));
        QTimer::singleShot(10'000, reply, [reply]() {
            if (reply->isRunning()) reply->abort();
        });
        QObject::connect(reply, &QNetworkReply::finished, &dialog, [&dialog, reply, nam,
                                                                             latitude, longitude, detectButton]() {
            detectButton->setEnabled(true);
            detectButton->setText(QStringLiteral("Detect from IP"));
            if (reply->error() != QNetworkReply::NoError) {
                QMessageBox::warning(&dialog, QStringLiteral("Detect from IP"),
                                     QStringLiteral("Geolocation lookup failed: %1").arg(reply->errorString()));
            } else {
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(reply->readAll(), &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                    QMessageBox::warning(&dialog, QStringLiteral("Detect from IP"),
                                         QStringLiteral("Geolocation response was not valid JSON"));
                } else {
                    const auto obj = document.object();
                    const auto lat = obj.value(QStringLiteral("latitude")).toDouble(-1000.0);
                    const auto lon = obj.value(QStringLiteral("longitude")).toDouble(-1000.0);
                    const auto okLat = lat >= -90.0 && lat <= 90.0;
                    const auto okLon = lon >= -180.0 && lon <= 180.0;
                    if (okLat) latitude->setValue(lat);
                    if (okLon) longitude->setValue(lon);
                    if (!okLat || !okLon) {
                        QMessageBox::warning(&dialog, QStringLiteral("Detect from IP"),
                                             QStringLiteral("Response did not include valid latitude/longitude"));
                    } else {
                        QMessageBox::information(&dialog, QStringLiteral("Detect from IP"),
                                                 QStringLiteral("Approximate position set to %1, %2 (city-level, not GPS-grade).")
                                                     .arg(lat, 0, 'f', 4).arg(lon, 0, 'f', 4));
                    }
                }
            }
            reply->deleteLater();
            nam->deleteLater();
        });
    });
    form->addRow(QStringLiteral("Frames kept in memory"), frames);
    form->addRow(QStringLiteral("Volumes loaded for playback"), volumes);
    form->addRow(QStringLiteral("Network contact"), contact);
    form->addRow(QStringLiteral("Parallel radar chunks"), parallelChunks);
    form->addRow(QStringLiteral("Request timeout"), timeout);
    form->addRow(QStringLiteral("Chunk retries"), retries);
    form->addRow(QStringLiteral("Initial retry backoff"), backoff);
    form->addRow(QStringLiteral("Radar refresh"), radarRefresh);
    form->addRow(QStringLiteral("Warning/report refresh"), overlayRefresh);
    form->addRow(QStringLiteral("GLM refresh"), lightningRefresh);
    form->addRow(QStringLiteral("GLM initial history"), lightningBackfill);
    form->addRow(QStringLiteral("GLM cell size"), lightningCell);
    form->addRow(QStringLiteral("GLM minimum rate"), lightningRate);
    form->addRow(QStringLiteral("GLM jump threshold"), lightningSigma);
    form->addRow(QStringLiteral("GLM sigma window"), lightningWindow);
    form->addRow(QStringLiteral("Map memory cache (restart)"), memoryCache);
    form->addRow(QStringLiteral("Map disk cache (restart)"), diskCache);
    form->addRow(QStringLiteral("Multi-panel"), synchronize);
    layout->addLayout(form);

    // ---- Alerts section --------------------------------------------------
    auto* alertsGroup = new QGroupBox(QStringLiteral("Alerts"), &dialog);
    auto* alertsLayout = new QFormLayout(alertsGroup);
    auto* alertsEnabled = new QCheckBox(QStringLiteral("Enable warning and lightning jump alerts"), &dialog);
    // Sound, notification, severity, age, and geo filter live in the sidebar
    // Layers group; only the advanced knobs remain here.
    auto* alertsEventFilter = new QCheckBox(QStringLiteral("Only alert on selected NWS event types"), &dialog);
    auto* alertsEventList = new QLineEdit(&dialog);
    alertsEventList->setPlaceholderText(
        QStringLiteral("Comma-separated, e.g. Tornado Warning, Severe Thunderstorm Warning, Flash Flood Warning"));
    auto* alertsSigma = new QDoubleSpinBox(&dialog);
    alertsSigma->setRange(1.0, 10.0);
    alertsSigma->setSuffix(QStringLiteral(" sigma"));
    alertsSigma->setSingleStep(0.25);
    auto* alertsSuppression = new QSpinBox(&dialog);
    alertsSuppression->setRange(1, 240);
    alertsSuppression->setSuffix(QStringLiteral(" min"));
    alertsSuppression->setSingleStep(1.0);
    alertsSuppression->setToolTip(
        QStringLiteral("Repeat alerts for the same warning/cell only after this long."));
    auto* alertsSoundPath = new QLineEdit(&dialog);
    alertsSoundPath->setPlaceholderText(QStringLiteral("(uses system beep when empty)"));
    auto* alertsBrowseButton = new QToolButton(&dialog);
    alertsBrowseButton->setText(QStringLiteral("Browse…"));
    auto* alertsTestButton = new QPushButton(QStringLiteral("Test alert"), &dialog);

    // Populate from AlertCenter's current config.
    if (alertCenter_) {
        const auto cfg = alertCenter_->config();
        alertsEnabled->setChecked(cfg.enabled);
        alertsEventFilter->setChecked(cfg.eventFilterEnabled);
        alertsEventList->setText(cfg.eventFilter.join(QStringLiteral(", ")));
        alertsSigma->setValue(cfg.lightningSigmaThreshold);
        alertsSuppression->setValue(cfg.repeatSuppressionMinutes);
        alertsSoundPath->setText(cfg.customSoundPath);
    }

    auto* alertsSoundRow = new QHBoxLayout();
    alertsSoundRow->addWidget(alertsSoundPath, 1);
    alertsSoundRow->addWidget(alertsBrowseButton);
    auto* alertsSoundContainer = new QWidget(&dialog);
    alertsSoundContainer->setLayout(alertsSoundRow);

    alertsLayout->addRow(alertsEnabled);
    alertsLayout->addRow(alertsEventFilter);
    alertsLayout->addRow(alertsEventFilter);
    alertsLayout->addRow(QStringLiteral("Event filter list"), alertsEventList);
    alertsLayout->addRow(QStringLiteral("Lightning jump threshold"), alertsSigma);
    alertsLayout->addRow(QStringLiteral("Suppress repeats for"), alertsSuppression);
    alertsLayout->addRow(QStringLiteral("Custom sound (.wav)"), alertsSoundContainer);
    alertsLayout->addRow(alertsTestButton);
    layout->addWidget(alertsGroup);

    connect(alertsBrowseButton, &QToolButton::clicked, &dialog, [&dialog, alertsSoundPath]() {
        const auto path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Choose alert sound"),
                                                       {}, QStringLiteral("WAV files (*.wav)"));
        if (!path.isEmpty()) alertsSoundPath->setText(path);
    });
    connect(alertsTestButton, &QPushButton::clicked, &dialog, [this]() {
        if (alertCenter_) alertCenter_->triggerTestAlert();
    });

    auto* alertNote = new QLabel(QStringLiteral(
        "Alerts are deduplicated by NWS alert id and lightning cell key for the suppression window. "
        "The first sync after startup only records what is already active without notifying, and "
        "warnings older than the age limit are logged silently. "
        "The geo filter uses the selected radar's location, or the chase point when chase mode is on."),
        &dialog);
    alertNote->setWordWrap(true);
    layout->addWidget(alertNote);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    chaseMode_ = chaseCheck->isChecked();
    applyTheme(appThemeFromInt(themeCombo->currentData().toInt()));
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
    networkSettings.setValue(QStringLiteral("lightning/refreshSeconds"), lightningRefresh->value());
    networkSettings.setValue(QStringLiteral("lightning/backfillMinutes"), lightningBackfill->value());
    networkSettings.setValue(QStringLiteral("lightning/cellSizeDeg"), lightningCell->value());
    networkSettings.setValue(QStringLiteral("lightning/minFlashRate"), lightningRate->value());
    networkSettings.setValue(QStringLiteral("lightning/sigmaThreshold"), lightningSigma->value());
    networkSettings.setValue(QStringLiteral("lightning/sigmaWindowMinutes"), lightningWindow->value());
    networkSettings.setValue(QStringLiteral("cache/memoryMbPerPanel"), memoryCache->value());
    networkSettings.setValue(QStringLiteral("cache/diskMbPerPanel"), diskCache->value());
    refreshTimer_->setInterval(radarRefresh->value() * 1000);
    overlayRefreshTimer_->setInterval(overlayRefresh->value() * 1000);
    glmSource_->configure(glmConfigFromSettings());
    glmSource_->refresh();
    for (const auto& session : radarSessions_) {
        while (session->history.size() > static_cast<std::size_t>(maximumHistoryFrames_))
            session->history.erase(session->history.begin());
    }
    saveSettings();
    if (chaseMode_) selectNearestChaseRadar();
    if (alertCenter_) {
        alertCenter_->setChaseMode(chaseMode_, chaseLatitude_, chaseLongitude_);
        pushProximityFilterToCanvases();
        AlertConfig cfg = alertCenter_->config();
        cfg.enabled = alertsEnabled->isChecked();
        cfg.eventFilterEnabled = alertsEventFilter->isChecked();
        cfg.lightningSigmaThreshold = alertsSigma->value();
        cfg.repeatSuppressionMinutes = alertsSuppression->value();
        cfg.customSoundPath = alertsSoundPath->text().trimmed();
        QStringList events;
        for (const auto& token : alertsEventList->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const auto trimmed = token.trimmed();
            if (!trimmed.isEmpty()) events.push_back(trimmed);
        }
        cfg.eventFilter = events;
        alertCenter_->setConfig(cfg);
        alertCenter_->clearMemory(); // so old suppressions don't carry over the new config silently
    }
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
    mapGrid_->setRowStretch(1, layoutIndex == 1 ? 0 : 1);
    mapGrid_->setColumnStretch(0, 1);
    mapGrid_->setColumnStretch(1, layoutIndex == 2 ? 0 : 1);
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
    updateCrossSectionAvailability();
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
    refreshActivePanelOverlayUi();
}

void MainWindow::refreshActivePanelOverlayUi() {
    if (!activePanelWarnings_) return; // not yet built
    const auto index = map_ ? panelIndex(map_) : -1;
    if (index < 0) {
        activePanelOverlayLabel_->setText(QStringLiteral("(no active panel)"));
        for (auto* c : {activePanelWarnings_, activePanelReports_, activePanelLightning_, activePanelFusion_}) {
            const QSignalBlocker blocker(c);
            c->setChecked(false);
            c->setEnabled(false);
        }
        useGlobalButton_->setEnabled(false);
        return;
    }
    auto* panel = maps_.at(index);
    activePanelOverlayLabel_->setText(QStringLiteral("Panel %1 (radar %2)").arg(index + 1).arg(panel->radarId()));
    const auto overrides = panel->overlayOverrides();
    auto warningsValue = overrides ? overrides->warnings : warningsCheck_->isChecked();
    auto reportsValue = overrides ? overrides->stormReports : reportsCheck_->isChecked();
    auto lightningValue = overrides ? overrides->lightning : lightningCheck_->isChecked();
    auto fusionValue = overrides ? overrides->lightningFusion : lightningFusionCheck_->isChecked();
    for (auto* c : {activePanelWarnings_, activePanelReports_, activePanelLightning_, activePanelFusion_}) {
        const QSignalBlocker blocker(c);
        c->setEnabled(true);
    }
    activePanelWarnings_->setChecked(warningsValue);
    activePanelReports_->setChecked(reportsValue);
    activePanelLightning_->setChecked(lightningValue);
    activePanelFusion_->setChecked(fusionValue);
    useGlobalButton_->setEnabled(overrides.has_value());
    refreshLayerBadges();
}

void MainWindow::syncProductChips() {
    if (productChips_.isEmpty() || !productCombo_) return;
    auto* itemModel = qobject_cast<QStandardItemModel*>(productCombo_->model());
    const auto current = productCombo_->currentIndex();
    for (int i = 0; i < productChips_.size(); ++i) {
        QSignalBlocker blocker(productChips_.at(i));
        productChips_.at(i)->setChecked(i == current);
        const auto* item = itemModel ? itemModel->item(i) : nullptr;
        productChips_.at(i)->setEnabled(!item || item->isEnabled());
    }
}

void MainWindow::rebuildTiltGrid() {
    if (!tiltGridBox_ || !tiltCombo_ || !tiltMinus_ || !tiltPlus_ || !tiltValueButton_) return;
    auto* layout = qobject_cast<QGridLayout*>(tiltGridBox_->layout());
    if (!layout) return;
    QLayoutItem* child = nullptr;
    while ((child = layout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }
    tiltGridButtons_.clear();
    const auto enabled = tiltCombo_->isEnabled() && tiltCombo_->count() > 0;
    tiltMinus_->setEnabled(enabled);
    tiltPlus_->setEnabled(enabled);
    tiltValueButton_->setEnabled(enabled);
    tiltValueButton_->setText(enabled ? tiltCombo_->currentText() : QStringLiteral("No tilts"));
    if (!enabled) {
        tiltGridBox_->hide();
        return;
    }
    int row = 0;
    int column = 0;
    for (int i = 0; i < tiltCombo_->count(); ++i) {
        auto* button = new QToolButton(tiltGridBox_);
        button->setObjectName(QStringLiteral("tiltGridButton"));
        const auto text = tiltCombo_->itemText(i);
        const auto cut = text.indexOf(QChar(0x00b0));
        button->setText(cut > 0 ? text.left(cut + 1) : text);
        button->setCheckable(true);
        button->setChecked(i == tiltCombo_->currentIndex());
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(button, &QToolButton::clicked, this, [this, i] { tiltCombo_->setCurrentIndex(i); });
        layout->addWidget(button, row, column);
        tiltGridButtons_.push_back(button);
        if (++column > 3) {
            column = 0;
            ++row;
        }
    }
}

void MainWindow::refreshLayerBadges() {
    if (!map_) return;
    if (warningsBadge_) warningsBadge_->setText(QString::number(map_->warningCount()));
    if (reportsBadge_) reportsBadge_->setText(QString::number(map_->stormReportCount()));
    if (placefilesBadge_) placefilesBadge_->setText(QString::number(map_->placefileElementCount()));
    if (lightningBadge_) {
        if (!cappedweather::glmL2ReaderAvailable() || !lightningCheck_ ||
            !lightningCheck_->isChecked()) {
            lightningBadge_->setText(QStringLiteral("off"));
        } else {
            const auto count = map_->lightningCellCount();
            lightningBadge_->setText(count > 0 ? QString::number(count)
                                              : QStringLiteral("idle"));
        }
    }
    if (fusionBadge_) {
        const bool on = lightningFusionCheck_ && lightningFusionCheck_->isChecked() &&
                        lightningFusionCheck_->isEnabled();
        fusionBadge_->setText(on ? QStringLiteral("on") : QStringLiteral("off"));
    }
}

void MainWindow::refreshScopeVisibility() {
    if (!perPanelBox_ || !scopeCombo_) return;
    const bool perPanel = scopeCombo_->currentIndex() == 1;
    perPanelBox_->setVisible(perPanel);
    if (perPanel) refreshActivePanelOverlayUi();
    else refreshLayerBadges();
}

void MainWindow::pushAlertDockToCenter() {
    if (!alertCenter_ || !severityCombo_ || !ageSpin_ || !soundCheck_ || !trayCheck_) return;
    AlertConfig cfg = alertCenter_->config();
    cfg.minSeverity = severityCombo_->currentData().toInt();
    cfg.maxWarningAgeMinutes = ageSpin_->value();
    cfg.soundEnabled = soundCheck_->isChecked();
    cfg.systemNotificationEnabled = trayCheck_->isChecked();
    alertCenter_->setConfig(cfg);
}

void MainWindow::refreshAlertDockFromCenter() {
    if (!alertCenter_ || !severityCombo_ || !ageSpin_ || !soundCheck_ || !trayCheck_) return;
    const auto cfg = alertCenter_->config();
    const QSignalBlocker severityBlocker(severityCombo_);
    const QSignalBlocker ageBlocker(ageSpin_);
    const QSignalBlocker soundBlocker(soundCheck_);
    const QSignalBlocker trayBlocker(trayCheck_);
    const auto severityIndex = severityCombo_->findData(cfg.minSeverity);
    severityCombo_->setCurrentIndex(severityIndex >= 0 ? severityIndex : 2);
    ageSpin_->setValue(cfg.maxWarningAgeMinutes);
    soundCheck_->setChecked(cfg.soundEnabled);
    trayCheck_->setChecked(cfg.systemNotificationEnabled);
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
    if (velocityBox_) velocityBox_->setVisible(velocityProduct);
    velocityModeCombo_->setEnabled(velocityProduct);
    const auto derived = velocityProduct && velocityModeCombo_->currentIndex() != 0;
    nyquistSpin_->setEnabled(derived);
    const auto stormRelative = velocityProduct && velocityModeCombo_->currentIndex() == 2;
    stormDirectionSpin_->setEnabled(stormRelative);
    stormSpeedSpin_->setEnabled(stormRelative);
}

void MainWindow::updateCrossSectionAvailability() {
    if (!crossSectionButton_ || !map_) return;
    const auto available = !isDerivedProduct(map_->product());
    if (!available && crossSectionButton_->isChecked()) crossSectionButton_->setChecked(false);
    crossSectionButton_->setEnabled(available);
    crossSectionButton_->setToolTip(
        available ? QStringLiteral("Draw a vertical cross-section through all tilts")
                  : QStringLiteral("Cross-sections require a native tilt product"));
}

void MainWindow::showCrossSection(const QPointF& start, const QPointF& end) {
    if (isDerivedProduct(map_->product())) return;
    const auto session = activeSession();
    if (!session || session->frameIndex < 0 ||
        session->frameIndex >= static_cast<int>(session->history.size())) return;
    const auto row = model_->rowForRadar(selectedRadarId_);
    if (row < 0) return;
    const auto& site = model_->siteAt(row);
    if (!site.latitude || !site.longitude) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("%1 Cross-section").arg(selectedRadarId_));
    dialog.resize(980, 620);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
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

void MainWindow::openLocalPlacefile() {
    const auto paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Open GR placefile"), {},
        QStringLiteral("GR placefiles (*.txt *.placefile);;Text files (*.txt);;All files (*.*)"));
    if (paths.isEmpty()) return;
    QStringList failures;
    for (const auto& path : paths) {
        QString error;
        if (!placefileSource_->addLocalFile(path, &error))
            failures.push_back(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), error));
    }
    clearPlacefilesButton_->setEnabled(placefileSource_->sourceCount() > 0);
    if (!failures.isEmpty())
        QMessageBox::warning(this, QStringLiteral("Placefile import"), failures.join(QLatin1Char('\n')));
}

void MainWindow::addPlacefileUrl() {
    bool accepted = false;
    const auto text = QInputDialog::getText(
        this, QStringLiteral("Add placefile URL"),
        QStringLiteral("HTTP(S) URL (refresh cadence is read from the placefile):"),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || text.isEmpty()) return;
    QString error;
    const QUrl url = QUrl::fromUserInput(text);
    if (!placefileSource_->addUrl(url, &error)) {
        QMessageBox::warning(this, QStringLiteral("Placefile URL"), error);
        return;
    }
    if (!placefileUrlContainsCredentials(url)) {
        const auto stored = url.adjusted(QUrl::NormalizePathSegments | QUrl::RemoveUserInfo |
                                         QUrl::RemoveFragment).toString();
        if (!remotePlacefileUrls_.contains(stored)) remotePlacefileUrls_.push_back(stored);
        QSettings().setValue(QStringLiteral("placefiles/urls"), remotePlacefileUrls_);
    } else {
        statusBar()->showMessage(
            QStringLiteral("Credential-bearing placefile URL added for this session only"), 6000);
    }
    clearPlacefilesButton_->setEnabled(true);
}

void MainWindow::clearPlacefiles() {
    placefileSource_->clear();
    remotePlacefileUrls_.clear();
    QSettings().remove(QStringLiteral("placefiles/urls"));
    clearPlacefilesButton_->setEnabled(false);
}

void MainWindow::prepareArchiveCase(const int requestedFrames) {
    stopAnimation();
    liveButton_->setChecked(false);
    cancelFor(selectedRadarId_);
    archiveSource_->cancel();
    maximumHistoryFrames_ = std::max(maximumHistoryFrames_, std::clamp(requestedFrames, 2, 24));
    if (const auto session = activeSession()) {
        session->pendingVolumeId.clear();
        session->pendingVolume = {};
        session->pendingChunkCount = 0;
        session->pendingFailedChunks = 0;
        session->pendingDecodeTasks = 0;
        session->pendingVolumeNetworkReady = false;
        session->derivingVolumeIds.clear();
        session->history.clear();
        session->frameIndex = -1;
        session->followLatest = false;
        session->requestActive = false;
        session->historyRequested = false;
        session->historyLoadStarted = false;
        session->playWhenHistoryReady = false;
    }
    updatePlaybackControls();
}

void MainWindow::openArchiveFiles() {
    if (selectedRadarId_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Level II archive"),
                                 QStringLiteral("Select a radar before loading its Archive-II files."));
        return;
    }
    auto paths = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Open NOAA Archive-II Level II files"), {},
        QStringLiteral("NEXRAD Level II (*_V03 *_V04 *_V05 *_V06 *_V07 *_V08);;All files (*.*)"));
    if (paths.isEmpty()) return;
    std::sort(paths.begin(), paths.end());
    if (paths.size() > 24) paths = paths.sliced(paths.size() - 24);
    prepareArchiveCase(paths.size());
    archiveSource_->loadFiles(selectedRadarId_, paths);
}

void MainWindow::openArchiveFolder() {
    if (selectedRadarId_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Level II archive"),
                                 QStringLiteral("Select a radar before loading an Archive-II folder."));
        return;
    }
    const auto path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open NOAA Archive-II Level II folder"));
    if (path.isEmpty()) return;
    const QDir directory(path);
    QStringList paths;
    for (const auto& file : directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name))
        paths.push_back(file.absoluteFilePath());
    if (paths.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Level II archive"),
                                 QStringLiteral("The selected folder contains no files."));
        return;
    }
    if (paths.size() > 24) paths = paths.sliced(paths.size() - 24);
    prepareArchiveCase(paths.size());
    archiveSource_->loadFiles(selectedRadarId_, paths);
}

void MainWindow::openAwsArchive() {
    if (selectedRadarId_.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("AWS Level II archive"),
                                 QStringLiteral("Select a radar before choosing an archive window."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Public AWS Level II archive"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* note = new QLabel(
        QStringLiteral("Load NOAA/Unidata public Archive-II volumes for %1. Times are UTC.")
            .arg(selectedRadarId_), &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* form = new QFormLayout();
    auto* start = new QDateTimeEdit(QDateTime::currentDateTimeUtc().addSecs(-6 * 3600), &dialog);
    start->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"));
    start->setCalendarPopup(true);
    auto* end = new QDateTimeEdit(QDateTime::currentDateTimeUtc(), &dialog);
    end->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"));
    end->setCalendarPopup(true);
    auto* maximum = new QSpinBox(&dialog);
    maximum->setRange(1, 24);
    maximum->setValue(24);
    maximum->setToolTip(QStringLiteral(
        "Matching volumes are evenly sampled into the 24-frame in-memory case timeline"));
    form->addRow(QStringLiteral("Start"), start);
    form->addRow(QStringLiteral("End"), end);
    form->addRow(QStringLiteral("Maximum volumes"), maximum);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto startUtc = start->dateTime().toUTC();
    const auto endUtc = end->dateTime().toUTC();
    if (startUtc > endUtc) {
        QMessageBox::warning(this, QStringLiteral("AWS Level II archive"),
                             QStringLiteral("The start time must not be after the end time."));
        return;
    }
    prepareArchiveCase(maximum->value());
    archiveSource_->requestAws(selectedRadarId_, startUtc, endUtc, maximum->value());
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::setRadarHealth(RadarHealthState state, const QString& tooltip) {
    if (radarHealthState_ == state && (tooltip.isEmpty() || (healthDot_ && healthDot_->toolTip() == tooltip))) return;
    radarHealthState_ = state;
    if (!healthDot_ || !healthLabel_) return;
    QString color;
    QString text;
    switch (state) {
        case RadarHealthState::Online:  color = QStringLiteral("#6fbf5a"); text = QStringLiteral("Online");  break;
        case RadarHealthState::Stale:   color = QStringLiteral("#e0c14a"); text = QStringLiteral("Stale");   break;
        case RadarHealthState::Offline: color = QStringLiteral("#d96d6d"); text = QStringLiteral("Offline"); break;
        case RadarHealthState::Unknown: default: color = QStringLiteral("#7a7d85"); text = QStringLiteral("Unknown"); break;
    }
    healthDot_->setStyleSheet(QStringLiteral(
        "background-color: %1; border: 1px solid #2b2d33; border-radius: 6px;").arg(color));
    healthLabel_->setText(text);
    if (!tooltip.isEmpty()) {
        healthDot_->setToolTip(tooltip);
        healthLabel_->setToolTip(tooltip);
    }
}

RadarHealthState MainWindow::classifyHealth() const {
    if (selectedRadarId_.isEmpty()) return RadarHealthState::Unknown;
    if (radarStatus_.contains(QStringLiteral("error"), Qt::CaseInsensitive)) return RadarHealthState::Offline;
    if (!lastVolumeTime_.isValid()) return RadarHealthState::Stale;
    const auto ageSec = lastVolumeTime_.secsTo(QDateTime::currentDateTimeUtc());
    if (ageSec <= 300) return RadarHealthState::Online;     // <= 5 min
    if (ageSec <= 900) return RadarHealthState::Stale;      // 5-15 min
    return RadarHealthState::Offline;
}

void MainWindow::recomputeRadarHealth() {
    setRadarHealth(classifyHealth());
}

bool MainWindow::isOperaRadar(const QString& radarId) const {
    if (!model_ || radarId.isEmpty()) return false;
    const auto row = model_->rowForRadar(radarId);
    if (row < 0) return false;
    return model_->siteAt(row).dataSource == "opera";
}

void MainWindow::requestLatestFor(const QString& radarId) {
    if (isOperaRadar(radarId)) operaSource_->requestLatest(radarId);
    else source_->requestLatest(radarId);
}

void MainWindow::requestRecentFor(const QString& radarId, const int volumeCount) {
    if (isOperaRadar(radarId)) operaSource_->requestRecent(radarId, volumeCount);
    else source_->requestRecent(radarId, volumeCount);
}

void MainWindow::cancelFor(const QString& radarId) {
    if (isOperaRadar(radarId)) operaSource_->cancel(radarId);
    else source_->cancel(radarId);
}

void MainWindow::clearDownloadedFor(const QString& radarId) {
    if (isOperaRadar(radarId)) operaSource_->clearDownloaded(radarId);
    else source_->clearDownloaded(radarId);
}

void MainWindow::updatePanelAttribution() {
    static const QString operaCredit =
        QStringLiteral("OPERA data CC BY 4.0 EUMETNET");
    for (auto* panel : maps_) {
        panel->setSupplementalAttribution(isOperaRadar(panel->radarId()) ? operaCredit
                                                                         : QString{});
    }
}

void MainWindow::pushProximityFilterToCanvases() {    if (!proximityFilterCheck_ || !proximityFilterRadius_) return;
    std::optional<QPointF> anchor;
    if (proximityFilterCheck_->isChecked()) {
        if (chaseMode_) {
            anchor = QPointF(chaseLongitude_, chaseLatitude_);
        } else if (!selectedRadarId_.isEmpty()) {
            const auto row = model_->rowForRadar(selectedRadarId_);
            if (row >= 0) {
                const auto& site = model_->siteAt(row);
                if (site.latitude && site.longitude) {
                    anchor = QPointF(*site.longitude, *site.latitude);
                }
            }
        }
    }
    const auto radius = proximityFilterRadius_->value();
    for (auto* panel : maps_) panel->setProximityFilter(anchor, radius);
    // The sidebar owns the geo filter for both display and alerting now.
    if (alertCenter_) {
        AlertConfig cfg = alertCenter_->config();
        cfg.geographicFilterEnabled = proximityFilterCheck_->isChecked();
        cfg.geographicFilterRadiusKm = radius;
        alertCenter_->setConfig(cfg);
    }
}

QVector<LayoutPreset> MainWindow::builtInPresets() const {
    QVector<LayoutPreset> presets;
    {
        LayoutPreset p;
        p.name = QStringLiteral("Chase 1x1");
        p.layoutIndex = 0; // 1x1
        p.product[0] = static_cast<int>(ProductKind::Reflectivity);
        p.tilt[0] = -1;
        p.builtIn = true;
        presets.push_back(p);
    }
    {
        LayoutPreset p;
        p.name = QStringLiteral("Analysis 2x2 dual-pol");
        p.layoutIndex = 3; // 2x2
        p.product[0] = static_cast<int>(ProductKind::Reflectivity);
        p.product[1] = static_cast<int>(ProductKind::Velocity);
        p.product[2] = static_cast<int>(ProductKind::CorrelationCoefficient);
        p.product[3] = static_cast<int>(ProductKind::DifferentialReflectivity);
        for (int i = 0; i < 4; ++i) p.tilt[i] = -1;
        p.builtIn = true;
        presets.push_back(p);
    }
    {
        LayoutPreset p;
        p.name = QStringLiteral("Lightning scout 1x2");
        p.layoutIndex = 1; // 1x2
        p.product[0] = static_cast<int>(ProductKind::Reflectivity);
        p.product[1] = static_cast<int>(ProductKind::Reflectivity);
        p.tilt[0] = -1;
        p.tilt[1] = -1;
        p.builtIn = true;
        presets.push_back(p);
    }
    return presets;
}

LayoutPreset MainWindow::presetByName(const QString& name) const {
    for (const auto& p : builtInPresets()) {
        if (p.name == name) return p;
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("presets/") + name);
    LayoutPreset p;
    p.name = name;
    p.layoutIndex = std::clamp(settings.value(QStringLiteral("layout"), 0).toInt(), 0, 3);
    for (int i = 0; i < 4; ++i) {
        p.product[i] = std::clamp(
            settings.value(QStringLiteral("panel%1/product").arg(i),
                           static_cast<int>(ProductKind::Reflectivity)).toInt(),
            0, static_cast<int>(kProductCount) - 1);
        p.tilt[i] = settings.value(QStringLiteral("panel%1/tilt").arg(i), -1).toInt();
    }
    settings.endGroup();
    p.builtIn = false;
    return p;
}

void MainWindow::rebuildPresetsCombo() {
    if (!presetsCombo_) return;
    const QSignalBlocker blocker(presetsCombo_);
    presetsCombo_->clear();
    presetsCombo_->addItem(QStringLiteral("(none)"), QString());
    for (const auto& p : builtInPresets()) {
        presetsCombo_->addItem(p.name, p.name);
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("presets"));
    const auto userPresets = settings.childGroups();
    settings.endGroup();
    QStringList userNames;
    for (const auto& g : userPresets) userNames.push_back(g);
    userNames.sort();
    for (const auto& name : userNames) {
        presetsCombo_->addItem(QStringLiteral("★ %1").arg(name), name);
    }
}

void MainWindow::applyLayoutPreset(const QString& name) {
    if (name.isEmpty()) return;
    const auto preset = presetByName(name);
    // 1. Switch the layout
    if (panelLayoutCombo_ && panelLayoutCombo_->currentIndex() != preset.layoutIndex) {
        const QSignalBlocker blocker(panelLayoutCombo_);
        panelLayoutCombo_->setCurrentIndex(preset.layoutIndex);
    }
    setPanelLayout(preset.layoutIndex);
    // 2. Apply per-panel product + tilt without changing radar sessions
    for (int i = 0; i < maps_.size(); ++i) {
        auto* panel = maps_.at(i);
        panel->setProduct(static_cast<ProductKind>(preset.product[i]));
        if (preset.tilt[i] > 0) panel->setElevationFilter(preset.tilt[i]);
        else panel->setElevationFilter(std::nullopt);
    }
    if (map_) {
        activateMap(map_);
        rebuildTiltCombo();
    }
    saveSettings();
}

void MainWindow::saveCurrentAsPreset() {
    bool ok = false;
    const auto suggested = QStringLiteral("My preset %1")
                                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HHmm")));
    const auto name = QInputDialog::getText(this, QStringLiteral("Save layout preset"),
                                            QStringLiteral("Preset name:"), QLineEdit::Normal,
                                            suggested, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (name == QStringLiteral("(none)")) return;
    for (const auto& p : builtInPresets()) {
        if (p.name.compare(name, Qt::CaseInsensitive) == 0) {
            QMessageBox::warning(this, QStringLiteral("Preset name"),
                                 QStringLiteral("'%1' is a built-in preset name; pick another.").arg(name));
            return;
        }
    }
    QSettings settings;
    settings.beginGroup(QStringLiteral("presets/") + name);
    settings.setValue(QStringLiteral("layout"),
                      panelLayoutCombo_ ? panelLayoutCombo_->currentIndex() : 0);
    for (int i = 0; i < maps_.size() && i < 4; ++i) {
        const auto* panel = maps_.at(i);
        settings.setValue(QStringLiteral("panel%1/product").arg(i), static_cast<int>(panel->product()));
        settings.setValue(QStringLiteral("panel%1/tilt").arg(i), panel->elevationFilter().value_or(-1));
    }
    settings.endGroup();
    rebuildPresetsCombo();
    // Re-select the saved preset by name.
    for (int i = 0; i < presetsCombo_->count(); ++i) {
        if (presetsCombo_->itemData(i).toString() == name) {
            const QSignalBlocker blocker(presetsCombo_);
            presetsCombo_->setCurrentIndex(i);
            break;
        }
    }
}

} // namespace cappedweather
