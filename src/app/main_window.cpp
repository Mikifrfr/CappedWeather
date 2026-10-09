#include "app/main_window.hpp"

#include "app/nexrad_source.hpp"
#include "app/opera_source.hpp"
#include "app/glm_source.hpp"
#include "app/http_client.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/session_store.hpp"
#include "app/session_prefetch.hpp"
#include "app/weather_overlay_source.hpp"
#include "app/spc_convective.hpp"
#include "app/metar_source.hpp"
#include "app/placefile_source.hpp"
#include "app/archive_source.hpp"
#include "core/derived_products.hpp"
#include "core/vad.hpp"
#include "app/case_pack.hpp"
#include "app/offline_pack.hpp"
#include "app/cross_section_widget.hpp"
#include "app/command_palette.hpp"
#include "app/timeline_strip.hpp"
#include "core/volume_spill.hpp"
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
#include <QListWidget>
#include <QTabWidget>
#include <QFormLayout>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QProgressDialog>
#include <QTemporaryDir>
#include <QProcess>
#include <QBuffer>
#include <QResizeEvent>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStandardPaths>
#if defined(CAPPEDWEATHER_HAS_MULTIMEDIA)
#include <QMediaCaptureSession>
#include <QMediaRecorder>
#include <QWindowCapture>
#endif
#include <QSlider>
#include <QStandardItemModel>
#include <QSpinBox>
#include <QStatusBar>
#include <QThread>
#include <QThreadPool>
#include <QTableWidget>
#include <QHeaderView>
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
#include <functional>
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

// Session history/spill moved to session_store.{hpp,cpp}; see MainWindow::frameChunk delegates below.

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
        case ProductKind::CompositeReflectivity:
            return QColor(QStringLiteral("#4ade80"));
        case ProductKind::Cappi:
            return QColor(QStringLiteral("#a3e635"));
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

// Greedy word wrap for hover text. Plain-text tooltips never wrap on their
// own, which renders as one screen-wide strip; rich-text wrapping is
// DPI-dependent, so break manually at word boundaries instead.
QString wrapTooltipText(const QString& text, const int columns = 88) {
    QString out;
    const auto paragraphs = text.split('\n');
    for (int p = 0; p < paragraphs.size(); ++p) {
        int count = 0;
        for (const auto& word : paragraphs.at(p).split(' ', Qt::SkipEmptyParts)) {
            if (count > 0 && count + word.size() + 1 > columns) {
                out += '\n';
                count = 0;
            } else if (count > 0) {
                out += ' ';
                count += 1;
            }
            out += word;
            count += word.size();
        }
        if (p + 1 < paragraphs.size()) out += '\n';
    }
    return out;
}

// One-line "what it is" per product, for tight spaces (palette detail).
QString productShortBlurb(const ProductKind kind) {
    switch (kind) {
        case ProductKind::Reflectivity:
            return QStringLiteral("Precipitation intensity and size");
        case ProductKind::Velocity:
            return QStringLiteral("Motion toward or away from the radar");
        case ProductKind::SpectrumWidth:
            return QStringLiteral("Velocity spread inside one gate");
        case ProductKind::DifferentialReflectivity:
            return QStringLiteral("Target shape: flat rain vs tumbling hail");
        case ProductKind::CorrelationCoefficient:
            return QStringLiteral("Target uniformity; low means a mix or debris");
        case ProductKind::DifferentialPhase:
            return QStringLiteral("Phase shift along the beam (heavy rain tracer)");
        case ProductKind::EchoTop:
            return QStringLiteral("Highest 18 dBZ altitude: updraft strength");
        case ProductKind::VerticallyIntegratedLiquid:
            return QStringLiteral("Column liquid: hail and downburst potential");
        case ProductKind::CompositeReflectivity:
            return QStringLiteral("Strongest echo in the column, all tilts");
        case ProductKind::Cappi:
            return QStringLiteral("Reflectivity at one constant altitude");
    }
    return {};
}

// Full hover explainer per product: what it measures, how to read it, and
// the thresholds that matter. Chips show only abbreviations, so this tooltip
// (plus the palette detail and the spelled-out label) is where the product
// gets explained.
QString productExplainer(const ProductKind kind) {
    switch (kind) {
        case ProductKind::Reflectivity:
            return QStringLiteral(
                "Returned power: precipitation intensity and size. Higher means heavier "
                "rain, hail, or debris. Rough scale: under 20 dBZ light, 35+ moderate, "
                "50+ severe, 60+ likely hail. A bright core inside weak echo can be debris.");
        case ProductKind::Velocity:
            return QStringLiteral(
                "Motion directly toward (cool colors) or away (warm) from the radar: "
                "only the radial component, never the full wind. Tight red/green "
                "couplets mark rotation. Values wrap past the Nyquist velocity, "
                "use Dealiased or storm-relative mode to unwrap.");
        case ProductKind::SpectrumWidth:
            return QStringLiteral(
                "Spread of velocities inside one gate. High values mean turbulence, "
                "strong shear, or debris: tornadic debris shows very high width "
                "collocated with low correlation coefficient.");
        case ProductKind::DifferentialReflectivity:
            return QStringLiteral(
                "Horizontal vs vertical return: big flat raindrops read high, tumbling "
                "hail and debris read near zero. Separates rain from hail and debris, "
                "and doubles as a calibration check in light rain.");
        case ProductKind::CorrelationCoefficient:
            return QStringLiteral(
                "How uniform the targets in a gate are, 0 to 1. Below about 0.9 means "
                "a mix: debris, melting hail, clutter, or biology. Low values inside "
                "high reflectivity are the classic debris-ball signature.");
        case ProductKind::DifferentialPhase:
            return QStringLiteral(
                "Accumulated phase shift along the beam. Only the downrange gradient "
                "matters: a steep rise tracks heavy rain. Absolute values carry no "
                "direct meaning.");
        case ProductKind::EchoTop:
            return QStringLiteral(
                "Highest altitude of the 18 dBZ echo: updraft strength at a glance. "
                "Above about 35 kft means a vigorous updraft. Derived from the volume, "
                "not measured directly.");
        case ProductKind::VerticallyIntegratedLiquid:
            return QStringLiteral(
                "Radar-estimated liquid in the column. High values flag hail and "
                "downburst potential, but hail contaminates the estimate (capped at "
                "56 dBZ). Derived from the volume, not measured directly.");
        case ProductKind::CompositeReflectivity:
            return QStringLiteral(
                "Strongest reflectivity found at any tilt over each point: storm "
                "structure and hail cores at a glance. Reads hotter than any "
                "single tilt by construction. Derived from the volume, not "
                "measured directly.");
        case ProductKind::Cappi:
            return QStringLiteral(
                "Reflectivity interpolated to one constant altitude (MSL, set "
                "below): compares storms without tilt geometry getting in the "
                "way. Below the lowest beam it shows the lowest tilt; above "
                "the scan top there is no data. Derived, not measured directly.");
    }
    return {};
}

// Chip hover text: full name plus the explainer. Chips show only the
// standard operational abbreviation (REF, VEL, ZDR, ...), which is what
// every operator already reads on a NWS display and which fits a
// two-column grid — so the tooltip is where the product gets explained.
QString productChipTooltip(const ProductKind kind) {
    const auto& descriptor = productDescriptor(kind);
    const QString head = QStringLiteral("%1 (%2) — %3")
                             .arg(QString::fromUtf8(descriptor.label.data()),
                                  QString::fromUtf8(descriptor.unit.data()),
                                  QString::fromUtf8(descriptor.shortName.data()));
    auto text = head + QStringLiteral("\n") + productExplainer(kind);
    if (kind == ProductKind::Cappi)
        text += QStringLiteral("\nAltitude selector appears below when CAPPI is active.");
    if (kind == ProductKind::CompositeReflectivity)
        text += QStringLiteral("\nColumn maximum over all tilts; reads hotter than any single tilt.");
    return wrapTooltipText(text);
}

// Reflectivity ramp swatch for the palette selector. Plain QWidget (no
// signals), so no Q_OBJECT/moc is needed: MainWindow drives it via
// setPalette() on combo change, custom import, and settings restore.
class PalettePreview : public QWidget {
public:
    explicit PalettePreview(QWidget* parent = nullptr)
        : QWidget(parent) {
        setFixedHeight(14);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setToolTip(QStringLiteral("Reflectivity ramp for the selected palette"));
        setAccessibleName(QStringLiteral("Palette preview"));
    }
    void setPalette(const RadarMapCanvas::PaletteKind kind,
                    QVector<RadarMapCanvas::PaletteStop> stops) {
        kind_ = kind;
        stops_ = std::move(stops);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto& descriptor = productDescriptor(ProductKind::Reflectivity);
        const int w = width();
        for (int x = 0; x < w; ++x) {
            const double t = w <= 1 ? 0.0 : static_cast<double>(x) / (w - 1);
            const double value = descriptor.minimum + t * (descriptor.maximum - descriptor.minimum);
            painter.setPen(RadarMapCanvas::colorForValue(ProductKind::Reflectivity, value, kind_,
                                                         100, stops_));
            painter.drawLine(x, 0, x, height());
        }
        painter.setPen(QColor(0, 0, 0, 90));
        painter.drawRect(0, 0, w - 1, height() - 1);
    }

private:
    RadarMapCanvas::PaletteKind kind_ = RadarMapCanvas::PaletteKind::Capped;
    QVector<RadarMapCanvas::PaletteStop> stops_;
};

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      model_(new RadarListModel(this)),
      source_(new NexradSource(this)),
      operaSource_(new OperaSource(this)),
      glmSource_(new GlmSource(this)),
      overlaySource_(new WeatherOverlaySource(this)),
      spcSource_(new SpcConvectiveSource(this)),
      metarSource_(new MetarSource(this)),
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
        panel->installEventFilter(this);
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
        // Linked probe: one panel's hovered geography becomes a ghost readout
        // on every other visible panel. Each target samples its own product
        // and volume, so REF vs VEL can never cross-contaminate.
        connect(panel, &RadarMapCanvas::probeMoved, this,
                [this, panel](const QPointF& lonLat) {
                    for (auto* target : maps_) {
                        if (target == panel || target->isHidden()) continue;
                        const auto sample = target->sampleAtGeo(lonLat);
                        if (!sample.valid) {
                            target->clearLinkedProbe();
                            continue;
                        }
                        const auto& descriptor = productDescriptor(target->product());
                        const auto productLabel =
                            QString::fromUtf8(descriptor.label.data()).section(QLatin1Char(' '), 0, 0);
                        target->setLinkedProbe(lonLat, sample.valueText,
                                               QStringLiteral("%1 %2").arg(target->radarId(), productLabel));
                    }
                });
        connect(panel, &RadarMapCanvas::probeHidden, this, [this, panel] {
            for (auto* target : maps_) {
                if (target == panel) continue;
                target->clearLinkedProbe();
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
        refreshCappiBox();
    });
    connect(tiltCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](const int index) {
        if (index >= 0 && tiltCombo_->itemData(index).isValid()) {
            map_->setElevationFilter(tiltCombo_->itemData(index).toInt());
        }
    });
    connect(refreshTimer_, &QTimer::timeout, this, &MainWindow::requestSelectedRadar);
    connect(overlayRefreshTimer_, &QTimer::timeout, overlaySource_, &WeatherOverlaySource::refresh);
    connect(overlayRefreshTimer_, &QTimer::timeout, spcSource_, &SpcConvectiveSource::refresh);
    connect(overlayRefreshTimer_, &QTimer::timeout, metarSource_, &MetarSource::refresh);
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
    connect(shearCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setShearVisible(visible);
        refreshLayerBadges();
    });
    connect(tvsCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setTvsVisible(visible);
        refreshLayerBadges();
    });
    connect(hailCoreCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setHailVisible(visible);
        refreshLayerBadges();
    });
    connect(zdrColumnCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setZdrVisible(visible);
        refreshLayerBadges();
    });
    connect(tdsCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setTdsVisible(visible);
        refreshLayerBadges();
    });
    connect(placefilesCheck_, &QCheckBox::toggled, this, [this](const bool visible) {
        for (auto* panel : maps_) panel->setPlacefilesVisible(visible);
    });
    connect(spcOutlookCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setSpcOutlookVisible(visible);
        refreshLayerBadges();
    });
    connect(mcdCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setMcdVisible(visible);
        refreshLayerBadges();
    });
    connect(metarCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setMetarVisible(visible);
        refreshLayerBadges();
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
    connect(source_, &NexradSource::volumeReady, this,
            [this](const QString& radarId, const QString& volumeId, int chunkCount,
                   int failedChunks, const QDateTime& receivedAt) {
                volumeReady(radarId, volumeId, chunkCount, failedChunks, receivedAt, true);
            });
    connect(operaSource_, &OperaSource::statusChanged, this, &MainWindow::sourceStatusChanged);
    connect(operaSource_, &OperaSource::requestFailed, this, &MainWindow::sourceFailed);
    connect(operaSource_, &OperaSource::volumeStarted, this, &MainWindow::volumeStarted);
    connect(operaSource_, &OperaSource::volumeProgress, this, &MainWindow::volumeProgress);
    connect(operaSource_, &OperaSource::chunkDecoded, this, &MainWindow::chunkDecoded);
    connect(operaSource_, &OperaSource::volumeReady, this,
            [this](const QString& radarId, const QString& volumeId, int chunkCount,
                   int failedChunks, const QDateTime& receivedAt) {
                volumeReady(radarId, volumeId, chunkCount, failedChunks, receivedAt, true);
            });
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
    connect(archiveSource_, &ArchiveSource::volumeReady, this,
            [this](const QString& radarId, const QString& volumeId, int chunkCount,
                   int failedChunks, const QDateTime& receivedAt) {
                // Case loads assemble through the same pipeline but must not
                // move the live health clocks: their timestamps are history.
                volumeReady(radarId, volumeId, chunkCount, failedChunks, receivedAt, false);
            });
    connect(overlaySource_, &WeatherOverlaySource::overlaysReady, this,
            [this](QVector<WeatherWarning> warnings, QVector<StormReport> reports, const QDateTime&) {
                for (auto* panel : maps_) panel->setWeatherOverlays(warnings, reports);
                if (alertCenter_) alertCenter_->ingestWarnings(warnings);
                latestWarnings_ = std::move(warnings);
                refreshWarningList();
                refreshTimelineStrip();
                refreshLayerBadges();
            });
    connect(overlaySource_, &WeatherOverlaySource::statusChanged, this, [this](const QString& status) {
        overlayStatus_ = status;
        updateUnifiedStatus();
    });
    connect(spcSource_, &SpcConvectiveSource::outlooksReady, this,
            [this](QVector<SpcOutlookFeature> outlooks) {
                for (auto* panel : maps_) panel->setSpcOutlooks(outlooks);
                refreshLayerBadges();
            });
    connect(spcSource_, &SpcConvectiveSource::mcdsReady, this, [this](QVector<SpcMcd> mcds) {
        for (auto* panel : maps_) panel->setMcds(mcds);
        refreshLayerBadges();
    });
    connect(spcSource_, &SpcConvectiveSource::statusChanged, this, [this](const QString& status) {
        spcStatus_ = status;
        updateUnifiedStatus();
    });
    connect(metarSource_, &MetarSource::metarsReady, this, [this](QVector<MetarObs> metars) {
        for (auto* panel : maps_) panel->setMetars(metars);
        refreshLayerBadges();
    });
    connect(metarSource_, &MetarSource::statusChanged, this, [this](const QString& status) {
        statusBar()->showMessage(status, 8000);
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
    // Painted cards take no Qt focus, so Up/Down scroll the active panel's
    // pinned card whenever one overflows. Editing widgets keep their keys:
    // combos, spinners, sliders, and lists navigate natively when focused.
    const auto scrollActiveCard = [this](const int pixels) {
        if (!map_ || !map_->hasScrollablePinnedCard()) return;
        const auto* focused = QApplication::focusWidget();
        if (focused &&
            (qobject_cast<const QComboBox*>(focused) || qobject_cast<const QAbstractSpinBox*>(focused) ||
             qobject_cast<const QSlider*>(focused) || qobject_cast<const QListWidget*>(focused) ||
             qobject_cast<const QLineEdit*>(focused))) {
            return;
        }
        map_->scrollPinnedCard(pixels);
    };
    addShortcut(QKeySequence(Qt::Key_Up), [scrollActiveCard] { scrollActiveCard(60); });
    addShortcut(QKeySequence(Qt::Key_Down), [scrollActiveCard] { scrollActiveCard(-60); });
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
    addShortcut(QKeySequence(Qt::Key_B), [this] { toggleFrameBookmark(); });
    addShortcut(QKeySequence(QStringLiteral("Ctrl+K")), [this] { showCommandPalette(); });
    // Products 1-9 take digit keys, the 10th takes 0 (GR-style).
    for (int product = 0; product < productCombo_->count() && product < 10; ++product) {
        const auto key = product < 9 ? Qt::Key_1 + product : Qt::Key_0;
        addShortcut(QKeySequence(key), [this, product] { productCombo_->setCurrentIndex(product); });
    }
    addShortcut(QKeySequence(QStringLiteral("?")), [this] {
        auto* dialog = new ShortcutHelpDialog(this);
        dialog->setModal(false);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
    addShortcut(QKeySequence(Qt::Key_F1), [this] {
        auto* dialog = new ShortcutHelpDialog(this);
        dialog->setModal(false);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
    addShortcut(QKeySequence(Qt::Key_Escape), [this] {
        if (map_) {
            if (!map_->infoCardRect().isEmpty()) map_->clearPinnedOverlay();
            else {
                map_->setMeasureMode(false);
                map_->setCrossSectionMode(false);
                if (measureButton_) measureButton_->setChecked(false);
                if (crossSectionButton_) crossSectionButton_->setChecked(false);
            }
        }
    });
    addShortcut(QKeySequence(Qt::Key_F11), [this] {
        if (maximizePanelButton_) maximizePanelButton_->toggle();
        else togglePanelMaximized();
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
    spcSource_->refresh();
    metarSource_->refresh();
    overlayRefreshTimer_->start(std::clamp(settings.value(QStringLiteral("network/overlayRefreshSeconds"), 60).toInt(),
                                           30, 600) * 1000);
    glmSource_->configure(glmConfigFromSettings());
    glmSource_->start();
    if (!restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray())) {
        if (const auto* screen = QGuiApplication::primaryScreen()) {
            const auto available = screen->availableGeometry();
            // Chase laptops are 1440p/4K now: use most of the screen instead of
            // a postage stamp. Small screens still clamp to a usable minimum.
            const bool large = available.width() >= 1400 || available.height() >= 850;
            const auto width = large
                ? std::clamp(static_cast<int>(available.width() * 0.90), 1100, 1800)
                : std::clamp(static_cast<int>(available.width() * 0.82), 900, 1180);
            const auto height = large
                ? std::clamp(static_cast<int>(available.height() * 0.88), 700, 1050)
                : std::clamp(static_cast<int>(available.height() * 0.84), 620, 780);
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
    qApp->setStyleSheet(appStylesheet(theme, fontScale_));
    if (mapGrid_ != nullptr) mapGrid_->setSpacing(theme == AppTheme::Prototype ? 6 : 4);
    const auto accent = appAccent(theme);
    for (auto* panel : maps_) {
        if (panel != nullptr) panel->setActiveBorderColor(accent);
    }
    update();
}

void MainWindow::applyFontScale(const double scale) {
    fontScale_ = std::clamp(scale, 0.8, 1.5);
    setHudFontScale(fontScale_);
    QFont appFont = QGuiApplication::font();
    appFont.setPointSizeF(std::max(1.0, uiBasePointSize_ * fontScale_));
    QGuiApplication::setFont(appFont);
    // Re-issue the theme sheet so the pinned chrome sizes (dock titles,
    // badges, tabs) follow the same scale as the app font and map HUD.
    qApp->setStyleSheet(appStylesheet(theme_, fontScale_));
    for (auto* panel : maps_) {
        if (panel != nullptr) panel->update();
    }
    update();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("CappedWeather - Level II Radar"));

    QFont uiFont = QGuiApplication::font();
    uiFont.setFamilies({QStringLiteral("IBM Plex Sans"), QStringLiteral("Segoe UI"),
                        QStringLiteral("Noto Sans"), QStringLiteral("DejaVu Sans")});
    QGuiApplication::setFont(uiFont);
    uiBasePointSize_ = uiFont.pointSizeF() > 0.0 ? uiFont.pointSizeF() : 9.0;
    QFont monoFont = uiFont;
    monoFont.setFamilies({QStringLiteral("IBM Plex Mono"), QStringLiteral("Cascadia Mono"),
                          QStringLiteral("Consolas"), QStringLiteral("monospace")});

    applyTheme(theme_);

    auto* mapHost = new QWidget(this);
    mapHost->setObjectName(QStringLiteral("mapHost"));
    mapGrid_ = new QGridLayout(mapHost);
    mapGrid_->setContentsMargins(0, 0, 0, 0);
    // Themed gutter (see applyTheme). This used to hardcode 4 here, which
    // silently overwrote the 6 px the Squall theme had just installed.
    mapGrid_->setSpacing(theme_ == AppTheme::Prototype ? 6 : 4);
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
        "Welcome to CappedWeather — live Level II in 3 steps:\n"
        "1. Pick a radar (search ID, city, or ★ favorites)   2. Keep LIVE on   3. Turn on Warnings + Reports\n"
        "Chasing? Settings → chase position → nearest radar. Press ? for shortcuts."));
    emptyStateHint_->setAlignment(Qt::AlignCenter);
    emptyStateHint_->setWordWrap(true);
    emptyStateHint_->setAccessibleName(QStringLiteral("Getting started"));
    // Styling lives in the theme sheet. A per-widget setStyleSheet outranks the
    // application sheet, so the inline version pinned this card to Squall colors
    // forever and the theme rule was dead.
    emptyStateHint_->raise();
    emptyStateHint_->show();

    offlineBanner_ = new QLabel(mapHost);
    offlineBanner_->setObjectName(QStringLiteral("offlineBanner"));
    offlineBanner_->setText(QStringLiteral("OFFLINE — showing cached volumes + basemap"));
    offlineBanner_->setAlignment(Qt::AlignCenter);
    offlineBanner_->setAccessibleName(QStringLiteral("Offline status"));
    offlineBanner_->hide();

    controlDock_ = new QDockWidget(QStringLiteral("RADAR"), this);
    controlDock_->setObjectName(QStringLiteral("controlDock"));
    controlDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable |
                              QDockWidget::DockWidgetFloatable);
    controlDock_->setMinimumWidth(300);
    controlDock_->setAccessibleName(QStringLiteral("Radar controls"));
    controlDock_->setAccessibleDescription(
        QStringLiteral("Station, product, layers, view and tools. Close it to maximize map."));
    auto* controlScroll = new QScrollArea(controlDock_);
    controlScroll->setWidgetResizable(true);
    controlScroll->setFrameShape(QFrame::NoFrame);
    controlScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* controlBody = new QWidget(controlScroll);
    controlBody->setObjectName(QStringLiteral("dockBody"));
    auto* controlLayout = new QVBoxLayout(controlBody);
    controlLayout->setContentsMargins(12, 8, 12, 12);
    controlLayout->setSpacing(4);
    const auto makeGroup = [this](QVBoxLayout* pageLayout, const QString& title,
                              const QString& key) {
        auto* page = pageLayout->parentWidget();
        auto* header = new QToolButton(page);
        header->setObjectName(QStringLiteral("groupHeader"));
        header->setText(title);
        header->setCheckable(true);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        auto* body = new QWidget(page);
        auto* bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 2, 0, 8);
        bodyLayout->setSpacing(5);
        pageLayout->addWidget(header);
        pageLayout->addWidget(body);
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
        // The stylesheets target this property, not an object name: the badges
        // are named warningsBadge/reportsBadge/..., so a `QLabel#layerBadge`
        // rule could never match and they rendered as full-brightness body
        // text - louder than the checkbox label they belong to.
        badge->setProperty("layerBadge", true);
        badge->setAlignment(Qt::AlignCenter);
        return badge;
    };
    // A QComboBox sizes itself to its *longest item*, so a label like
    // "Severe (warnings only, default)" or "Native operational palette" sets a
    // hard minimum for the whole dock layout. The scroll area hides its
    // horizontal scrollbar and cannot shrink below that minimum, so everything
    // to the right was silently clipped. Pin these to a short contents length
    // and let the row's expanding policy hand them the leftover width; the
    // popup still opens at full item width.
    const auto makeShrinkableCombo = [](QComboBox* combo, const int characters) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(characters);
        combo->setMinimumWidth(0);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return combo;
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

    // Tabbed sidebar: one long scrolling column became four short ones, so
    // the dock stops scrolling on a laptop window. The pinned station card
    // above stays visible on every tab; groups keep their collapse state.
    sidebarTabs_ = new QTabWidget(controlBody);
    sidebarTabs_->setObjectName(QStringLiteral("sidebarTabs"));
    QVBoxLayout* radarPageLayout = nullptr;
    QVBoxLayout* layersPageLayout = nullptr;
    QVBoxLayout* displayPageLayout = nullptr;
    QVBoxLayout* alertsPageLayout = nullptr;
    const auto makeTabPage = [this, controlBody](const QString& title, QVBoxLayout*& pageLayout) {
        auto* page = new QWidget(controlBody);
        // Same background as the old flat body; the theme paints #dockBody.
        page->setObjectName(QStringLiteral("dockBody"));
        pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 4, 0, 0);
        pageLayout->setSpacing(4);
        sidebarTabs_->addTab(page, title);
    };
    makeTabPage(QStringLiteral("Radar"), radarPageLayout);
    makeTabPage(QStringLiteral("Layers"), layersPageLayout);
    makeTabPage(QStringLiteral("Display"), displayPageLayout);
    makeTabPage(QStringLiteral("Alerts"), alertsPageLayout);
    controlLayout->addWidget(sidebarTabs_, 1);
    connect(sidebarTabs_, &QTabWidget::currentChanged, this, [this](int index) {
        QSettings().setValue(QStringLiteral("ui/sidebarTab"), index);
    });

    auto* radarGroup = makeGroup(radarPageLayout, QStringLiteral("Radar"), QStringLiteral("radar"));
    auto* stationRow = new QHBoxLayout();
    stationRow->setSpacing(4);
    stationCombo_ = new QComboBox(controlBody);
    stationCombo_->setObjectName(QStringLiteral("stationCombo"));
    stationCombo_->setEditable(true);
    stationCombo_->setInsertPolicy(QComboBox::NoInsert);
    // A 24-character minimum plus AdjustToMinimumContentsLengthWithIcon forced
    // this combo to ~24 chars wide, which dragged the whole dock body past the
    // dock's own width. The scroll area hides its horizontal scrollbar, so
    // every row to the right of it silently got clipped: "Clear" lost its last
    // letter, the warning filter lost its right edge, and the opacity slider
    // handles were sliced in half. Let the expanding size policy fill the row
    // and keep only a small floor so the field never collapses to nothing.
    stationCombo_->setMinimumContentsLength(6);
    stationCombo_->setMinimumWidth(0);
    stationCombo_->lineEdit()->setPlaceholderText(QStringLiteral("Find radar, city, or state…"));
    stationCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    stationRow->addWidget(stationCombo_, 1);
    favoriteButton_ = new QToolButton(controlBody);
    favoriteButton_->setObjectName(QStringLiteral("favoriteRadarButton"));
    favoriteButton_->setText(QStringLiteral("☆"));
    favoriteButton_->setMinimumWidth(0);
    favoriteButton_->setCheckable(true);
    favoriteButton_->setToolTip(QStringLiteral("Favorite selected radar"));
    stationRow->addWidget(favoriteButton_);
    radarGroup->addLayout(stationRow);

    auto* productGroup = makeGroup(radarPageLayout, QStringLiteral("Product and tilt"), QStringLiteral("product"));
    // The combo stays the data owner (logic, persistence, tests address it);
    // the chip grid is its visible face. It is hidden, never removed.
    productCombo_ = new QComboBox(controlBody);
    productCombo_->setObjectName(QStringLiteral("productCombo"));
    for (const auto& product : productDescriptors()) {
        productCombo_->addItem(QString::fromUtf8(product.label.data()), static_cast<int>(product.kind));
    }
    productCombo_->hide();
    // Spells out the selected product in full. The chips carry the standard
    // 3-5 letter operational abbreviations, which is what fits two columns in
    // a 320 px dock; without this line the user had no readable full name
    // anywhere in the panel.
    activeProductLabel_ = new QLabel(controlBody);
    activeProductLabel_->setObjectName(QStringLiteral("activeProductLabel"));
    productGroup->addWidget(activeProductLabel_);
    auto* chipGrid = new QGridLayout();
    chipGrid->setSpacing(5);
    chipGrid->setObjectName(QStringLiteral("productChipGrid"));
    int chipRow = 0;
    int chipColumn = 0;
    for (const auto& product : productDescriptors()) {
        const auto kind = product.kind;
        auto* chip = new QToolButton(controlBody);
        chip->setObjectName(QStringLiteral("productChip"));
        chip->setText(QString::fromUtf8(product.shortName.data()));
        chip->setAccessibleName(QString::fromUtf8(product.label.data()));
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
    tiltMinus_->setToolTip(QStringLiteral("Previous tilt (PgUp)"));
    tiltPlus_ = new QToolButton(controlBody);
    tiltPlus_->setObjectName(QStringLiteral("tiltPlusButton"));
    tiltPlus_->setText(QStringLiteral("+"));
    tiltPlus_->setToolTip(QStringLiteral("Next tilt (PgDn)"));
    tiltValueButton_ = new QToolButton(controlBody);
    tiltValueButton_->setObjectName(QStringLiteral("tiltValueButton"));
    tiltValueButton_->setToolTip(QStringLiteral("Click for the full tilt list (PgUp/PgDn steps)"));
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
    // CAPPI altitude selector: same contextual pattern as the velocity box,
    // visible only while the active panel views CAPPI. The height is a
    // global MSL setting (the derived field lives on the shared chunk),
    // so changing it re-derives histories in the background.
    cappiBox_ = new QWidget(controlBody);
    cappiBox_->setObjectName(QStringLiteral("cappiBox"));
    auto* cappiBoxLayout = new QVBoxLayout(cappiBox_);
    cappiBoxLayout->setContentsMargins(0, 0, 0, 0);
    cappiBoxLayout->setSpacing(4);
    auto* cappiCaption = new QLabel(QStringLiteral("CAPPI height (MSL)"), controlBody);
    cappiCaption->setObjectName(QStringLiteral("groupCaption"));
    cappiBoxLayout->addWidget(cappiCaption);
    cappiHeightCombo_ = new QComboBox(controlBody);
    cappiHeightCombo_->setObjectName(QStringLiteral("cappiHeightCombo"));
    cappiHeightCombo_->addItem(QStringLiteral("2 km"), 2000);
    cappiHeightCombo_->addItem(QStringLiteral("3 km"), 3000);
    cappiHeightCombo_->addItem(QStringLiteral("4 km"), 4000);
    cappiHeightCombo_->addItem(QStringLiteral("5 km"), 5000);
    cappiHeightCombo_->addItem(QStringLiteral("7 km"), 7000);
    cappiHeightCombo_->setToolTip(QStringLiteral("Constant altitude, meters above sea level. Below the lowest beam shows the lowest tilt; above the scan top shows no data."));
    cappiBoxLayout->addWidget(cappiHeightCombo_);
    productGroup->addWidget(cappiBox_);
    cappiBox_->hide();
    connect(cappiHeightCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](const int index) {
                if (index >= 0 && cappiHeightCombo_->itemData(index).isValid())
                    setCappiHeightM(cappiHeightCombo_->itemData(index).toDouble());
            });

    auto* layersGroup = makeGroup(layersPageLayout, QStringLiteral("Layers"), QStringLiteral("layers"));
    auto* displayGroup = makeGroup(displayPageLayout, QStringLiteral("Display"), QStringLiteral("display"));
    auto* alertsGroup = makeGroup(alertsPageLayout, QStringLiteral("Alerts"), QStringLiteral("alerts"));
    // Warning inbox: the sorted worklist for the 181-warning days. Click a
    // row to fly the active panel to the polygon and pin its card; hovering
    // a row outlines the polygon on the map. Populated by refreshWarningList
    // from the overlaysReady snapshot.
    auto* inboxCaption = new QLabel(QStringLiteral("Active warnings"), controlBody);
    inboxCaption->setObjectName(QStringLiteral("groupCaption"));
    alertsGroup->addWidget(inboxCaption);
    warningList_ = new QListWidget(controlBody);
    warningList_->setObjectName(QStringLiteral("warningList"));
    warningList_->setMinimumHeight(120);
    warningList_->setMaximumHeight(220);
    warningList_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    warningList_->setMouseTracking(true);
    warningList_->viewport()->setMouseTracking(true);
    warningList_->viewport()->installEventFilter(this);
    warningList_->setToolTip(QStringLiteral("Warnings sorted by severity, then newest. Click to locate, hover to highlight."));
    alertsGroup->addWidget(warningList_);
    connect(warningList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item || !map_) return;
        map_->focusWarning(item->data(Qt::UserRole).toInt());
    });
    connect(warningList_, &QListWidget::itemEntered, this, [this](QListWidgetItem* item) {
        if (!map_) return;
        map_->setHighlightedWarning(item ? item->data(Qt::UserRole).toInt() : -1);
    });
    auto* scopeRow = new QHBoxLayout();
    auto* scopeLabel = new QLabel(QStringLiteral("Show layers on"), controlBody);
    scopeCombo_ = makeShrinkableCombo(new QComboBox(controlBody), 9);
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
    makeLayerRow(shearCheck_, QStringLiteral("Rotation / shear"),
                 QStringLiteral("shearCheck"),
                 QStringLiteral("Azimuthal-shear objects, lightning not required"), shearBadge_,
                 QStringLiteral("shearBadge"), true,
                 QStringLiteral("Thresholded rotation couplets from Level II velocity (pink = rotating)"));
    makeLayerRow(tvsCheck_, QStringLiteral("TVS / meso markers"),
                 QStringLiteral("tvsCheck"),
                 QStringLiteral("NSSL-style couplet + depth tracking"), tvsBadge_,
                 QStringLiteral("tvsBadge"), true,
                 QStringLiteral("Tornado-vortex and mesocyclone markers from gate-to-gate couplets with vertical and time continuity (red triangle = TVS, orange diamond = meso)"));
    makeLayerRow(hailCoreCheck_, QStringLiteral("Hail cores (VIL density)"),
                 QStringLiteral("hailCoreCheck"),
                 QStringLiteral("VIL density guidance, size ranges not diameters"), hailCoreBadge_,
                 QStringLiteral("hailCoreBadge"), true,
                 QStringLiteral("Hail guidance from VIL divided by echo-top height (3.5 severe possible, 4.0 likely, 4.5 significant). Correlates with hail size, never measures a diameter."));
    makeLayerRow(zdrColumnCheck_, QStringLiteral("ZDR columns (updraft)"),
                 QStringLiteral("zdrColumnCheck"),
                 QStringLiteral("Dual-pol updraft proxy, guidance only"), zdrColumnBadge_,
                 QStringLiteral("zdrColumnBadge"), true,
                 QStringLiteral("ZDR columns mark strong updrafts lofting big drops (ZDR 1+ dB in 40+ dBZ over 1.5+ km on 2+ tilts). No columns on single-pol volumes. Guidance only, never a hail size."));
    makeLayerRow(tdsCheck_, QStringLiteral("Debris signature (TDS)"),
                 QStringLiteral("tdsCheck"),
                 QStringLiteral("CC drop at a live couplet, persisted"), tdsBadge_,
                 QStringLiteral("tdsBadge"), true,
                 QStringLiteral("Polarimetric debris confirmation: low correlation coefficient collocated with a rotation couplet, shown only after repeating across volumes. Confirms lofted debris, never predicts."));
    makeLayerRow(placefilesCheck_, QStringLiteral("GR placefiles"), QStringLiteral("placefilesCheck"),
                 QStringLiteral("Chaser and spotter feeds"), placefilesBadge_,
                 QStringLiteral("placefilesBadge"), true,
                 QStringLiteral("Show loaded GR-compatible placefile markers and boundaries"));
    makeLayerRow(spcOutlookCheck_, QStringLiteral("SPC outlook"), QStringLiteral("spcOutlookCheck"),
                 QStringLiteral("Day 1 + 2 categorical risk"), spcOutlookBadge_,
                 QStringLiteral("spcOutlookBadge"), true,
                 QStringLiteral("Storm Prediction Center Day 1 and Day 2 categorical outlooks in official colors. Context, not a warning."));
    makeLayerRow(mcdCheck_, QStringLiteral("SPC discussions"), QStringLiteral("mcdCheck"),
                 QStringLiteral("Active mesoscale discussions"), mcdBadge_,
                 QStringLiteral("mcdBadge"), true,
                 QStringLiteral("Active SPC mesoscale discussions with watch probability. Tornado and severe watches already arrive through NWS alerts."));
    makeLayerRow(metarCheck_, QStringLiteral("METAR stations"), QStringLiteral("metarCheck"),
                 QStringLiteral("Surface temp/dew/wind near the radar"), metarBadge_,
                 QStringLiteral("metarBadge"), true,
                 QStringLiteral("Nearby METAR surface observations: temp/dew markers colored by flight category, full wind and age on click."));
    auto* placefileRow = new QHBoxLayout();
    placefileRow->setSpacing(4);
    // Share the row evenly and allow shrinking. These three default to their
    // sizeHint, so a narrow dock clipped the last one.
    const auto makeRowButton = [controlBody](const QString& text, const QString& object,
                                             const QString& tip) {
        auto* button = new QToolButton(controlBody);
        button->setObjectName(object);
        button->setText(text);
        button->setToolTip(tip);
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return button;
    };
    addPlacefileButton_ = makeRowButton(QStringLiteral("File…"),
                                        QStringLiteral("addPlacefileButton"),
                                        QStringLiteral("Open a local GR placefile"));
    placefileRow->addWidget(addPlacefileButton_);
    addPlacefileUrlButton_ = makeRowButton(QStringLiteral("URL…"),
                                           QStringLiteral("addPlacefileUrlButton"),
                                           QStringLiteral("Fetch a GR placefile URL with automatic refresh"));
    placefileRow->addWidget(addPlacefileUrlButton_);
    clearPlacefilesButton_ = makeRowButton(QStringLiteral("Clear"),
                                          QStringLiteral("clearPlacefilesButton"),
                                          QStringLiteral("Remove all loaded placefiles"));
    clearPlacefilesButton_->setEnabled(false);
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
    // Spin boxes size to their widest representable value ("1500.0 km"), which
    // is wide enough to push the label off a narrow dock row. An explicit
    // minimum width of 0 lets the row layout compress them instead.
    proximityFilterRadius_->setMinimumWidth(0);
    proximityFilterRadius_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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
    alertsGroup->addWidget(glmStatusNote_);
    warningFilterCombo_ = makeShrinkableCombo(new QComboBox(controlBody), 11);
    warningFilterCombo_->addItems({QStringLiteral("All warnings"), QStringLiteral("Tornado"),
                                   QStringLiteral("Severe Thunderstorm"), QStringLiteral("Flash Flood"),
                                   QStringLiteral("Extreme")});
    warningFilterCombo_->setToolTip(QStringLiteral("Filter visible warning polygons"));
    layersGroup->addWidget(warningFilterCombo_);
    connect(warningFilterCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        for (auto* panel : maps_)
            panel->setWarningFilter(index == 0 ? QString{} : warningFilterCombo_->itemText(index));
        refreshWarningList();
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
    displayGroup->addLayout(opacityRow);
    connect(radarOpacitySlider_, &QSlider::valueChanged, this, [this](int opacity) {
        for (auto* panel : maps_) panel->setRadarOpacity(opacity);
    });
    auto* warningOpacityRow = new QHBoxLayout();
    warningOpacityRow->setSpacing(8);
    auto* warningOpacityLabel = new QLabel(QStringLiteral("Warning opacity"), controlBody);
    warningOpacityLabel->setObjectName(QStringLiteral("section"));
    warningOpacityRow->addWidget(warningOpacityLabel);
    warningOpacitySlider_ = new QSlider(Qt::Horizontal, controlBody);
    warningOpacitySlider_->setObjectName(QStringLiteral("warningOpacitySlider"));
    warningOpacitySlider_->setRange(20, 100);
    warningOpacitySlider_->setValue(90);
    warningOpacitySlider_->setToolTip(
        QStringLiteral("Warning polygon opacity. Lower it when polygons hide the echo."));
    warningOpacityRow->addWidget(warningOpacitySlider_, 1);
    displayGroup->addLayout(warningOpacityRow);
    connect(warningOpacitySlider_, &QSlider::valueChanged, this, [this](int opacity) {
        for (auto* panel : maps_) panel->setWarningOpacity(opacity);
    });
    warningsOnTopCheck_ = new QCheckBox(QStringLiteral("Warnings above echo"), controlBody);
    warningsOnTopCheck_->setObjectName(QStringLiteral("warningsOnTopCheck"));
    warningsOnTopCheck_->setChecked(true);
    warningsOnTopCheck_->setToolTip(
        QStringLiteral("When off, radar echo paints over warning polygons (useful for debris-ball inspection)."));
    displayGroup->addWidget(warningsOnTopCheck_);
    connect(warningsOnTopCheck_, &QCheckBox::toggled, this, [this](bool onTop) {
        for (auto* panel : maps_) panel->setWarningsOnTop(onTop);
    });
    auto* paletteRow = new QHBoxLayout();
    paletteRow->setSpacing(4);
    paletteCombo_ = makeShrinkableCombo(new QComboBox(controlBody), 13);
    paletteCombo_->addItems({QStringLiteral("Capped palette"), QStringLiteral("Native operational palette"),
                             QStringLiteral("Color-safe palette"), QStringLiteral("Custom palette")});
    paletteRow->addWidget(paletteCombo_, 1);
    connect(paletteCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        for (auto* panel : maps_) panel->setPalette(static_cast<RadarMapCanvas::PaletteKind>(index));
    });
    auto* paletteButton = new QToolButton(controlBody);
    paletteButton->setObjectName(QStringLiteral("paletteButton"));
    paletteButton->setText(QStringLiteral("Palette…"));
    paletteButton->setToolTip(QStringLiteral("Import a value,color palette text file"));
    paletteButton->setMinimumWidth(0);
    paletteButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(paletteButton, &QToolButton::clicked, this, &MainWindow::importCustomPalette);
    paletteRow->addWidget(paletteButton);
    displayGroup->addLayout(paletteRow);
    palettePreview_ = new PalettePreview(controlBody);
    displayGroup->addWidget(palettePreview_);
    connect(paletteCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { refreshPalettePreview(); });
    belowThresholdCheck_ = new QCheckBox(QStringLiteral("Below threshold"), controlBody);
    belowThresholdCheck_->setObjectName(QStringLiteral("belowThresholdCheck"));
    displayGroup->addWidget(belowThresholdCheck_);
    connect(belowThresholdCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setBelowThresholdVisible(visible);
    });
    trailCheck_ = new QCheckBox(QStringLiteral("Storm trails (max-REF track)"), controlBody);
    trailCheck_->setObjectName(QStringLiteral("trailCheck"));
    trailCheck_->setChecked(true);
    trailCheck_->setToolTip(QStringLiteral("One honest point per volume: strongest reflectivity object, fading with age"));
    displayGroup->addWidget(trailCheck_);
    connect(trailCheck_, &QCheckBox::toggled, this, [this](bool visible) {
        for (auto* panel : maps_) panel->setStormTrailVisible(visible);
    });
    smoothingCheck_ = new QCheckBox(QStringLiteral("Smooth echo (display only)"), controlBody);
    smoothingCheck_->setObjectName(QStringLiteral("smoothingCheck"));
    smoothingCheck_->setChecked(false);
    smoothingCheck_->setToolTip(QStringLiteral("Display-only azimuth overlap to hide gate gaps. Gates stay honest when off."));
    displayGroup->addWidget(smoothingCheck_);
    connect(smoothingCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        for (auto* panel : maps_) panel->setDisplaySmoothing(enabled);
    });
    scanDiagnosticsLabel_ = new QLabel(QStringLiteral("Scan: —"), controlBody);
    scanDiagnosticsLabel_->setObjectName(QStringLiteral("layerSub"));
    scanDiagnosticsLabel_->setWordWrap(true);
    scanDiagnosticsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* scanRow = new QHBoxLayout();
    scanRow->addWidget(scanDiagnosticsLabel_, 1);
    auto* scanDetailsButton = new QToolButton(controlBody);
    scanDetailsButton->setText(QStringLiteral("Details"));
    scanDetailsButton->setToolTip(QStringLiteral("Per-tilt table plus VAD wind profile for the active volume"));
    connect(scanDetailsButton, &QToolButton::clicked, this, [this] { showScanDiagnostics(); });
    scanRow->addWidget(scanDetailsButton);
    displayGroup->addLayout(scanRow);
    displayPageLayout->addSpacing(2);

    // Alert tuning lives next to the inbox it governs, on the Alerts tab.
    auto* notifyCaption = new QLabel(QStringLiteral("Notify me about new items"), controlBody);
    notifyCaption->setObjectName(QStringLiteral("groupCaption"));
    alertsGroup->addWidget(notifyCaption);
    alertsGroup->addWidget(proximityFilterCheck_);
    alertsGroup->addLayout(proximityRow);
    auto* severityRow = new QHBoxLayout();
    auto* severityLabel = new QLabel(QStringLiteral("Minimum severity"), controlBody);
    severityCombo_ = makeShrinkableCombo(new QComboBox(controlBody), 11);
    severityCombo_->setObjectName(QStringLiteral("severityCombo"));
    severityCombo_->addItem(QStringLiteral("Minor (advisories + above)"), 0);
    severityCombo_->addItem(QStringLiteral("Moderate (watches + above)"), 1);
    severityCombo_->addItem(QStringLiteral("Severe (warnings only, default)"), 2);
    severityCombo_->addItem(QStringLiteral("Extreme (tornado / extreme wind only)"), 3);
    severityCombo_->setToolTip(QStringLiteral("Warnings below this NWS severity never notify"));
    severityRow->addWidget(severityLabel);
    severityRow->addWidget(severityCombo_, 1);
    alertsGroup->addLayout(severityRow);
    auto* ageRow = new QHBoxLayout();
    auto* ageLabel = new QLabel(QStringLiteral("Only if issued within"), controlBody);
    ageLabel->setToolTip(QStringLiteral("Older warnings are logged silently"));
    ageSpin_ = new QSpinBox(controlBody);
    ageSpin_->setObjectName(QStringLiteral("ageSpin"));
    ageSpin_->setRange(0, 1440);
    ageSpin_->setSuffix(QStringLiteral(" min"));
    ageSpin_->setSingleStep(5);
    ageSpin_->setSpecialValueText(QStringLiteral("any age"));
    ageSpin_->setMinimumWidth(0);
    ageSpin_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ageRow->addWidget(ageLabel);
    ageRow->addWidget(ageSpin_, 1);
    alertsGroup->addLayout(ageRow);
    soundCheck_ = new QCheckBox(QStringLiteral("Sound"), controlBody);
    soundCheck_->setObjectName(QStringLiteral("soundCheck"));
    alertsGroup->addWidget(soundCheck_);
    trayCheck_ = new QCheckBox(QStringLiteral("Tray notification"), controlBody);
    trayCheck_->setObjectName(QStringLiteral("trayCheck"));
    alertsGroup->addWidget(trayCheck_);
    connect(severityCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { pushAlertDockToCenter(); });
    connect(ageSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { pushAlertDockToCenter(); });
    connect(soundCheck_, &QCheckBox::toggled, this, [this] { pushAlertDockToCenter(); });
    connect(trayCheck_, &QCheckBox::toggled, this, [this] { pushAlertDockToCenter(); });

    velocityModeCombo_ = makeShrinkableCombo(new QComboBox(controlBody), 13);
    velocityModeCombo_->setObjectName(QStringLiteral("velocityModeCombo"));
    velocityModeCombo_->addItems({QStringLiteral("Native velocity"), QStringLiteral("Dealiased velocity"),
                                  QStringLiteral("Storm-relative velocity")});
    velocityModeCombo_->setToolTip(QStringLiteral("Native: as sent. Dealiased: unfolds folding past Nyquist. Storm-relative: subtracts storm motion."));
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

    auto* viewGroup = makeGroup(displayPageLayout, QStringLiteral("View"), QStringLiteral("view"));
    panelLayoutCombo_ = makeShrinkableCombo(new QComboBox(controlBody), 5);
    panelLayoutCombo_->setObjectName(QStringLiteral("panelLayoutCombo"));
    panelLayoutCombo_->addItems({QStringLiteral("1x1"), QStringLiteral("1x2"),
                                 QStringLiteral("2x1"), QStringLiteral("2x2")});
    panelLayoutCombo_->setToolTip(QStringLiteral("Radar panel layout"));
    viewGroup->addWidget(panelLayoutCombo_);
    connect(panelLayoutCombo_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MainWindow::setPanelLayout);
    auto* presetRow = new QHBoxLayout();
    presetsCombo_ = makeShrinkableCombo(new QComboBox(controlBody), 13);
    presetsCombo_->setObjectName(QStringLiteral("presetsCombo"));
    presetsCombo_->setToolTip(QStringLiteral("Named panel layout preset (built-in + user-saved)"));
    savePresetButton_ = new QToolButton(controlBody);
    savePresetButton_->setObjectName(QStringLiteral("savePresetButton"));
    savePresetButton_->setText(QStringLiteral("Save"));
    savePresetButton_->setToolTip(QStringLiteral("Capture the current layout + per-panel products as a new preset"));
    savePresetButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
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
    auto* panelHint = new QLabel(QStringLiteral("Click a panel to work on it."), controlBody);
    panelHint->setObjectName(QStringLiteral("groupCaption"));
    panelHint->setWordWrap(true);
    panelHint->setToolTip(QStringLiteral("Click a panel to work on it. Product, tilt, and tools follow. "
                                         "Double-click a panel to fill the window."));
    viewGroup->addWidget(panelHint);
    auto* viewButtonsRow = new QHBoxLayout();
    maximizePanelButton_ = new QToolButton(controlBody);
    maximizePanelButton_->setObjectName(QStringLiteral("maximizePanelButton"));
    maximizePanelButton_->setText(QStringLiteral("Maximize panel"));
    maximizePanelButton_->setCheckable(true);
    maximizePanelButton_->setToolTip(QStringLiteral("Fill the window with the active panel (double-click also works)"));
    maximizePanelButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    maximizePanelButton_->setMinimumHeight(28);
    viewButtonsRow->addWidget(maximizePanelButton_, 1);
    dataSaverButton_ = new QToolButton(controlBody);
    dataSaverButton_->setObjectName(QStringLiteral("dataSaverButton"));
    dataSaverButton_->setText(QStringLiteral("Data-saver"));
    dataSaverButton_->setToolTip(QStringLiteral("One-click chase preset: 2 parallel chunks, 4-frame history, "
                                                "2-minute overlay refresh. For flaky cellular."));
    dataSaverButton_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    dataSaverButton_->setMinimumHeight(28);
    viewButtonsRow->addWidget(dataSaverButton_, 1);
    viewGroup->addLayout(viewButtonsRow);
    connect(maximizePanelButton_, &QToolButton::toggled, this, [this](bool) { togglePanelMaximized(); });
    connect(dataSaverButton_, &QToolButton::clicked, this, &MainWindow::applyDataSaverPreset);

    auto* navigationGrid = new QGridLayout();
    navigationGrid->setSpacing(4);
    navigationGrid->setColumnStretch(0, 1);
    navigationGrid->setColumnStretch(1, 1);
    navigationGrid->setColumnStretch(2, 1);
    int navigationPosition = 0;
    const auto addViewButton = [controlBody, navigationGrid,
                                &navigationPosition](const QString& text, const QString& tip) {
        auto* button = new QToolButton(controlBody);
        button->setText(text);
        button->setToolTip(tip);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        button->setMinimumHeight(28);
        navigationGrid->addWidget(button, navigationPosition / 3, navigationPosition % 3);
        ++navigationPosition;
        return button;
    };
    auto* nationalButton = addViewButton(QStringLiteral("Home"), QStringLiteral("National view"));
    auto* recenterButton = addViewButton(QStringLiteral("Recenter"), QStringLiteral("Center selected radar (R)"));
    auto* zoomOutButton = addViewButton(QStringLiteral("Zoom -"), QStringLiteral("Zoom out (-)"));
    auto* zoomInButton = addViewButton(QStringLiteral("Zoom +"), QStringLiteral("Zoom in (+)"));
    connect(nationalButton, &QToolButton::clicked, this, [this] { map_->showNationalView(); });
    connect(recenterButton, &QToolButton::clicked, this, [this] { map_->recenterSelected(); });
    connect(zoomOutButton, &QToolButton::clicked, this, [this] { map_->zoomOut(); });
    connect(zoomInButton, &QToolButton::clicked, this, [this] { map_->zoomIn(); });
    auto* settingsButton = addViewButton(QStringLiteral("Settings"),
                                         QStringLiteral("Operational and chase settings"));
    connect(settingsButton, &QToolButton::clicked, this, &MainWindow::showSettingsDialog);
    viewGroup->addLayout(navigationGrid);

    auto* toolsGroup = makeGroup(displayPageLayout, QStringLiteral("Tools"), QStringLiteral("tools"));
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
    measureButton_ = addToolButton(QStringLiteral("Measure"),
                                      QStringLiteral("Measure distance and bearing (Esc cancels)"));
    measureButton_->setCheckable(true);
    measureButton_->setAccessibleName(QStringLiteral("Measure distance"));
    connect(measureButton_, &QToolButton::toggled, this, [this](bool enabled) {
        if (enabled && crossSectionButton_) crossSectionButton_->setChecked(false);
        map_->setMeasureMode(enabled);
    });
    crossSectionButton_ = addToolButton(QStringLiteral("Cross-section"),
                                       QStringLiteral("Draw a vertical cross-section — stays open beside live radar (Esc cancels)"));
    crossSectionButton_->setObjectName(QStringLiteral("crossSectionButton"));
    crossSectionButton_->setCheckable(true);
    connect(crossSectionButton_, &QToolButton::toggled, this, [this](bool enabled) {
        if (enabled) measureButton_->setChecked(false);
        map_->setCrossSectionMode(enabled);
    });
    toolsGroup->addLayout(toolsGrid);
    // One stretch per tab page, so each tab's groups pack to the top.
    radarPageLayout->addStretch(1);
    layersPageLayout->addStretch(1);
    displayPageLayout->addStretch(1);
    alertsPageLayout->addStretch(1);
    controlScroll->setWidget(controlBody);
    controlDock_->setWidget(controlScroll);
    addDockWidget(Qt::LeftDockWidgetArea, controlDock_);
    // Two-column chip/layer rows need ~320 px; without this the dock opens
    // at the bare minimum and clips them.
    resizeDocks({controlDock_}, {324}, Qt::Horizontal);

    playbackDock_ = new QDockWidget(QStringLiteral("PLAYBACK"), this);
    playbackDock_->setObjectName(QStringLiteral("playbackDock"));
    playbackDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable |
                               QDockWidget::DockWidgetFloatable);
    playbackDock_->setAccessibleName(QStringLiteral("Playback and archive"));
    playbackDock_->setAccessibleDescription(
        QStringLiteral("Timeline transport, export, and archive loading."));
    auto* playbackBody = new QWidget(playbackDock_);
    playbackBody->setObjectName(QStringLiteral("dockBody"));
    auto* playbackLayout = new QVBoxLayout(playbackBody);
    playbackLayout->setContentsMargins(8, 6, 8, 7);
    playbackLayout->setSpacing(4);
    // Filmstrip above the transport row: one block per loaded volume, red
    // ticks for warning onsets, amber caps for bookmarks.
    timelineStrip_ = new TimelineStrip(playbackBody);
    playbackLayout->addWidget(timelineStrip_);
    connect(timelineStrip_, &TimelineStrip::frameSelected, this, [this](int index) {
        const auto session = activeSession();
        if (!session) return;
        stopAnimation();
        session->followLatest = index >= static_cast<int>(session->history.size()) - 1 &&
                                liveButton_->isChecked();
        showFrame(index);
    });
    connect(timelineStrip_, &TimelineStrip::bookmarkToggled, this, [this](const QString& volumeId) {
        const auto session = activeSession();
        if (!session || volumeId.isEmpty()) return;
        if (session->bookmarks.contains(volumeId)) session->bookmarks.remove(volumeId);
        else session->bookmarks.insert(volumeId);
        QSettings().setValue(QStringLiteral("bookmarks/") + session->radarId,
                             session->bookmarks.values());
        refreshTimelineStrip();
    });
    auto* transportRow = new QHBoxLayout();
    transportRow->setContentsMargins(0, 0, 0, 0);
    transportRow->setSpacing(4);
    previousButton_ = new QToolButton(playbackBody);
    previousButton_->setObjectName(QStringLiteral("previousFrameButton"));
    previousButton_->setText(QStringLiteral("Previous"));
    previousButton_->setToolTip(QStringLiteral("Previous frame (Left arrow)"));
    transportRow->addWidget(previousButton_);
    playButton_ = new QToolButton(playbackBody);
    playButton_->setObjectName(QStringLiteral("playButton"));
    playButton_->setText(QStringLiteral("Play"));
    playButton_->setToolTip(QStringLiteral("Play / pause timeline loop (Space)"));
    playButton_->setCheckable(true);
    transportRow->addWidget(playButton_);
    nextButton_ = new QToolButton(playbackBody);
    nextButton_->setObjectName(QStringLiteral("nextFrameButton"));
    nextButton_->setText(QStringLiteral("Next"));
    nextButton_->setToolTip(QStringLiteral("Next frame (Right arrow)"));
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
    timelineButton->setToolTip(QStringLiteral("Export loaded frames as GIF (cancellable, with progress)"));
    connect(timelineButton, &QToolButton::clicked, this, &MainWindow::exportTimelineAnimation);
    detailsRow->addWidget(timelineButton);
    auto* mp4Button = new QToolButton(playbackBody);
    mp4Button->setObjectName(QStringLiteral("timelineMp4Button"));
    mp4Button->setText(QStringLiteral("Timeline MP4"));
    mp4Button->setToolTip(QStringLiteral("Record the timeline as MP4 video (realtime window capture)"));
    connect(mp4Button, &QToolButton::clicked, this, &MainWindow::exportTimelineMp4);
    detailsRow->addWidget(mp4Button);
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
    // No second resizeDocks here: an earlier 272 px call silently undid the
    // 324 px set above (and the comment explaining why), clipping the
    // two-column chip and layer rows that need the room.

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
    // The status bar narrows and widens as the permanent widgets come and go,
    // so the elision has to be recomputed, not computed once at build time.
    status->installEventFilter(this);

    setupMenus();
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

void MainWindow::applyUnifiedStatusElision() {
    if (!unifiedStatusLabel_ || unifiedStatusFullText_.isEmpty()) return;
    // QLabel hard-clips at the widget edge with no ellipsis, so the GLM tail
    // was routinely cut mid-word beside the permanent widgets. Elide to the
    // real available width, recomputed whenever the status bar resizes (window
    // resize, or the progress/retry/cancel widgets appearing).
    const auto available = std::max(120, unifiedStatusLabel_->width());
    QFont statusFont = unifiedStatusLabel_->font();
    statusFont.setPointSizeF(std::max(7.0, statusFont.pointSizeF() - 1.0));
    unifiedStatusLabel_->setText(
        QFontMetrics(statusFont).elidedText(unifiedStatusFullText_, Qt::ElideRight, available));
}

void MainWindow::updateUnifiedStatus() {
    if (!unifiedStatusLabel_) return;
    unifiedStatusFullText_ =
        QStringLiteral("Radar: %1    |    NWS/SPC: %2    |    GLM: %3")
            .arg(radarStatus_, overlayStatus_, lightningStatus_);
    unifiedStatusLabel_->setToolTip(unifiedStatusFullText_);
    applyUnifiedStatusElision();
    if (glmStatusNote_) {
        if (!cappedweather::glmL2ReaderAvailable()) {
            glmStatusNote_->setText(QStringLiteral("GLM disabled (netCDF-C unavailable)"));
            glmStatusNote_->setProperty("notice", true);
            glmStatusNote_->setVisible(true);
        } else if (lightningStatus_.startsWith(QStringLiteral("GLM disabled"))) {
            glmStatusNote_->setText(lightningStatus_);
            glmStatusNote_->setProperty("notice", true);
            glmStatusNote_->setVisible(true);
        } else {
            glmStatusNote_->setVisible(false);
            glmStatusNote_->setProperty("notice", false);
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
    for (const auto& bookmarked :
         QSettings().value(QStringLiteral("bookmarks/") + radarId).toStringList())
        session->bookmarks.insert(bookmarked);
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
    if (watched == statusBar() && event->type() == QEvent::Resize) {
        applyUnifiedStatusElision();
    }
    if (warningList_ && watched == warningList_->viewport() && event->type() == QEvent::Leave) {
        if (map_) map_->setHighlightedWarning(-1);
    }
    if (event->type() == QEvent::MouseButtonDblClick) {
        for (auto* panel : maps_) {
            if (watched == panel) {
                // Double-clicking the pinned detail card expands it instead
                // of maximizing the panel; everywhere else keeps the old
                // maximize gesture.
                const auto* mouse = static_cast<const QMouseEvent*>(event);
                if (panel->pinnedCardContains(mouse->position())) {
                    panel->togglePinnedCardExpanded();
                    return true;
                }
                activateMap(panel);
                if (maximizePanelButton_) maximizePanelButton_->toggle();
                else togglePanelMaximized();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (emptyStateHint_ && emptyStateHint_->isVisible()) {
        const auto* parent = emptyStateHint_->parentWidget();
        if (parent) {
            const auto hintWidth = std::min(560, parent->width() - 40);
            emptyStateHint_->adjustSize();
            const auto hintHeight = std::max(64, emptyStateHint_->sizeHint().height());
            emptyStateHint_->setGeometry(
                (parent->width() - hintWidth) / 2,
                (parent->height() - hintHeight) / 2,
                hintWidth,
                hintHeight);
        }
    }
    if (offlineBanner_ && offlineBanner_->isVisible()) {
        const auto* parent = offlineBanner_->parentWidget();
        if (parent) {
            const auto bannerWidth = std::min(460, parent->width() - 40);
            offlineBanner_->setGeometry(
                (parent->width() - bannerWidth) / 2, 10,
                bannerWidth, offlineBanner_->sizeHint().height());
            offlineBanner_->raise();
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
    lastArrivalTime_ = {};
    consecutiveFailures_ = 0;
    updateOfflineBanner();
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
            // Surface stations follow the radar: refresh the METAR region so
            // the markers stay local instead of stuck on the last site.
            metarSource_->setRegion(*site.latitude, *site.longitude, 3.0);
            metarSource_->refresh();
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
        ++consecutiveFailures_;
        updateOfflineBanner();
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
    session->pendingDecodedChunks = 0;
    session->pendingPartialRays = 0;
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
                              std::shared_ptr<DecodedChunk> decoded, const QDateTime&) {
    const auto session = sessionFor(radarId, false);
    if (!session || !decoded || volumeId != session->pendingVolumeId) return;
    acceptDecodedChunk(radarId, volumeId, std::move(decoded));
}

void MainWindow::volumeReady(const QString& radarId, const QString& volumeId, const int chunkCount,
                             const int failedChunks, const QDateTime& receivedAt, const bool live) {
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
    // First paint happens HERE, on the base volume: Echo Tops / VIL used to
    // run serially before anything reached the screen, so every load paid
    // the full derive cost up front. Derive now runs on a copy in the
    // background and swaps into the published frame when it lands; panels
    // showing REF/VEL never wait for products they aren't viewing.
    auto volume = std::make_shared<DecodedChunk>(std::move(session->pendingVolume));
    double radarElevationMeters = 0.0;
    const auto radarRow = model_->rowForRadar(radarId);
    if (radarRow >= 0) radarElevationMeters = model_->siteAt(radarRow).antennaElevationMeters;
    finalizeVolumeReady(radarId, volumeId, chunkCount, failedChunks, receivedAt, volume, live);
    // Derive only pays off for multi-tilt reflectivity volumes; single-tilt
    // TDWR/SCAN pieces can never grow Echo Tops, so skip the copy entirely.
    const bool multiTilt = chunkSupportsDerivedProducts(*volume);
    if (!multiTilt || session->derivingVolumeIds.contains(volumeId)) return;
    session->derivingVolumeIds.insert(volumeId);
    const QPointer<MainWindow> guard(this);
    const double cappiHeightM = cappiHeightM_;
    QThreadPool::globalInstance()->start(
        [guard, volume, radarId, volumeId, radarElevationMeters, cappiHeightM] {
            // Deep copy on the pool thread (never on the GUI thread), then
            // derive in place. The published base frame stays immutable and
            // readable the whole time.
            auto derived = std::make_shared<DecodedChunk>(*volume);
            appendDerivedProducts(*derived, radarElevationMeters, cappiHeightM);
            if (!guard) return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, radarId, volumeId, derived, cappiHeightM] {
                    if (guard) guard->swapDerivedVolume(radarId, volumeId, derived, cappiHeightM);
                },
                Qt::QueuedConnection);
        });
    if (radarId == selectedRadarId()) {
        radarStatus_ = QStringLiteral("%1 live · derived products (ET/VIL/CREF/CAPPI) in background").arg(radarId);
        updateUnifiedStatus();
    }
}

void MainWindow::swapDerivedVolume(const QString& radarId, const QString& volumeId,
                                   std::shared_ptr<DecodedChunk> derived,
                                   const double cappiHeightM) {
    const auto session = sessionFor(radarId, false);
    if (!session) return;
    session->derivingVolumeIds.remove(volumeId);
    if (!derived) return;
    const auto it = std::find_if(session->history.begin(), session->history.end(),
                                 [&volumeId](const RadarFrame& frame) {
                                     return frame.volumeId == volumeId;
                                 });
    // Evicted while deriving (history cap) or superseded: nothing to swap.
    if (it == session->history.end()) return;
    if (it->chunk) {
        it->chunk = std::move(derived);
        it->cappiHeightM = cappiHeightM;
    } else if (!it->spilledPath.isEmpty()) {
        // Frame spilled mid-derive: persist the derived superset over the
        // base spill so a later scrub still gets ET/VIL. Tag the altitude
        // only when the spill lands: a failed write must not mark stale
        // base data as fresh.
        if (!spillVolume(*derived, it->spilledPath.toStdString())) return;
        it->cappiHeightM = cappiHeightM;
        return;
    } else {
        return;
    }
    if (radarId == selectedRadarId() &&
        session->frameIndex >= 0 &&
        session->frameIndex < static_cast<int>(session->history.size()) &&
        session->history.at(static_cast<std::size_t>(session->frameIndex)).volumeId == volumeId) {
        // Refreshes chips (ET18/VIL enable now), tilt grid, badges, strip.
        showFrame(session->frameIndex);
    } else {
        for (auto* panel : maps_) {
            if (panel->radarId() != radarId) continue;
            panel->setDecodedChunk(it->chunk, it->dataTime);
        }
    }
}

std::shared_ptr<DecodedChunk> MainWindow::frameChunk(RadarSession& session,
                                                      const std::size_t index) {
    return sessionFrameChunk(session, index, maximumHistoryFrames_);
}

void MainWindow::enforceSpillBudget(RadarSession& session,
                                    const int excludeIndex) {
    enforceSessionSpillBudget(session, maximumHistoryFrames_, excludeIndex);
}

void MainWindow::scheduleSpillPrefetch() {
    // Every panel's session, not just the active one: mixed US+EU layouts
    // scrub each radar independently. Job keys dedupe shared sessions.
    QSet<QString> warmed;
    const auto warm = [&](const std::shared_ptr<RadarSession>& session) {
        if (!session || session->radarId.isEmpty() || warmed.contains(session->radarId)) return;
        warmed.insert(session->radarId);
        prefetchSessionSpills(session);
    };
    warm(activeSession());
    for (auto* panel : maps_) warm(sessionFor(panel->radarId(), false));
    // A scrub storm can leave finished keys behind if sessions were
    // replaced mid-flight; keep the set bounded.
    while (spillPrefetchInFlight_.size() > 32) spillPrefetchInFlight_.erase(spillPrefetchInFlight_.begin());
}

void MainWindow::prefetchSessionSpills(const std::shared_ptr<RadarSession>& session) {
    if (!session || session->history.empty() || session->frameIndex < 0) return;
    // Closest neighbours first; two is enough to cover scrub/next-playback
    // without stacking volume-sized reads on field hardware.
    const auto indexes = SessionPrefetcher::pickSpillIndexes(*session, session->frameIndex, 2);
    if (indexes.empty()) return;
    const QPointer<MainWindow> guard(this);
    const QString radarId = session->radarId;
    const int maxFrames = maximumHistoryFrames_;
    for (const int index : indexes) {
        if (index < 0 || index >= static_cast<int>(session->history.size())) continue;
        const auto& frame = session->history.at(static_cast<std::size_t>(index));
        if (frame.chunk || frame.spilledPath.isEmpty()) continue;
        const QString volumeId = frame.volumeId;
        const QString path = frame.spilledPath;
        const QString jobKey = radarId + QLatin1Char('/') + volumeId;
        if (spillPrefetchInFlight_.contains(jobKey)) continue;
        spillPrefetchInFlight_.insert(jobKey);
        QThreadPool::globalInstance()->start([guard, radarId, volumeId, path, jobKey, maxFrames] {
            auto loaded = loadSpilledVolume(path.toStdString());
            if (!guard) return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, radarId, volumeId, loaded, jobKey, maxFrames] {
                    if (!guard) return;
                    guard->spillPrefetchInFlight_.remove(jobKey);
                    if (!loaded) return;  // unreadable: scrub path clears it
                    const auto live = guard->sessionFor(radarId, false);
                    if (!live) return;
                    if (installPrefetchedChunk(*live, volumeId, loaded, maxFrames)) {
                        // Installed silently: no showFrame() here (that would
                        // recurse into another prefetch). The next scrub or
                        // playback step hits RAM instead of disk.
                    }
                },
                Qt::QueuedConnection);
        });
    }
}

void MainWindow::finalizeVolumeReady(const QString& radarId, const QString& volumeId,
                                     const int chunkCount, const int failedChunks,
                                     const QDateTime& receivedAt,
                                     std::shared_ptr<DecodedChunk> volume, const bool live) {
    const auto session = sessionFor(radarId, false);
    if (!session) return;
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
    } else {
        // Already published (e.g. a late duplicate delivery): the frame in
        // history wins, refresh the display and stop. Never showFrame() for
        // a radar that isn't selected — that would paint another session's
        // frame over the user's timeline position.
        if (isCurrentVolume) session->pendingVolumeId.clear();
        if (radarId == selectedRadarId() && session->frameIndex >= 0) showFrame(session->frameIndex);
        return;
    }
    std::sort(session->history.begin(), session->history.end(), [](const RadarFrame& left, const RadarFrame& right) {
        return left.dataTime == right.dataTime ? left.volumeId < right.volumeId
                                               : left.dataTime < right.dataTime;
    });
    // Oldest frames spill to disk instead of being dropped, so the timeline
    // outgrows RAM without losing volumes.
    enforceSpillBudget(*session);
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
        const auto chunk = frameChunk(*session, static_cast<std::size_t>(session->frameIndex));
        if (chunk) {
            const auto& frame = session->history.at(static_cast<std::size_t>(session->frameIndex));
            for (auto* panel : maps_) if (panel->radarId() == radarId) {
                panel->setDecodedChunk(chunk, frame.dataTime);
                panel->setVolumeQuality(std::max(0, frame.chunkCount - frame.failedChunks),
                                        frame.chunkCount, frame.failedChunks,
                                        chunk->rejectedMessages, chunk->rejectedBlocks);
            }
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
        // Health tracks the live pipeline only: archive case loads carry
        // days-old timestamps and must not paint the station Offline, and
        // backfilled history older than the newest frame must not move the
        // arrival clock either.
        if (live && (!lastVolumeTime_.isValid() || receivedAt > lastVolumeTime_)) {
            lastVolumeTime_ = receivedAt;
            lastArrivalTime_ = QDateTime::currentDateTimeUtc();
        }
        recomputeRadarHealth();
        consecutiveFailures_ = 0;
        updateOfflineBanner();
    }
}

void MainWindow::acceptDecodedChunk(const QString& radarId, const QString& volumeId,
                                    std::shared_ptr<DecodedChunk> decoded) {
    const auto session = sessionFor(radarId, false);
    if (!session || !decoded || volumeId != session->pendingVolumeId) return;
    session->pendingVolume.messageCount += decoded->messageCount;
    session->pendingVolume.rejectedMessages += decoded->rejectedMessages;
    session->pendingVolume.rejectedBlocks += decoded->rejectedBlocks;
    session->pendingVolume.rays.insert(session->pendingVolume.rays.end(),
                                       std::make_move_iterator(decoded->rays.begin()),
                                       std::make_move_iterator(decoded->rays.end()));
    ++session->pendingDecodedChunks;
    session->pendingDecodeTasks = std::max(0, session->pendingDecodeTasks - 1);
    // Progressive first paint: sweep the map as chunks land instead of
    // holding a spinner for the whole volume. First glimpse at 500 rays,
    // then every 4000 more. Snapshots are deep copies, but each step fires
    // at most once per volume and the copy is a few ms against seconds of
    // downloads. Guarded to follow-latest so scrubbing the timeline never
    // gets clobbered by a still-downloading volume.
    const auto rays = session->pendingVolume.rays.size();
    const bool firstGlimpse = session->pendingPartialRays == 0 && rays >= 500;
    const bool nextStep = session->pendingPartialRays > 0 &&
                          rays >= session->pendingPartialRays + 4000;
    if ((firstGlimpse || nextStep) &&
        (session->history.empty() || session->followLatest)) {
        session->pendingPartialRays = rays;
        auto snapshot = std::make_shared<DecodedChunk>(session->pendingVolume);
        for (auto* panel : maps_) {
            if (panel->radarId() != radarId) continue;
            panel->setDecodedChunk(snapshot, {});
            panel->setVolumeQuality(session->pendingDecodedChunks,
                                    std::max(session->pendingChunkCount, session->pendingDecodedChunks),
                                    -1, session->pendingVolume.rejectedMessages,
                                    session->pendingVolume.rejectedBlocks);
        }
    }
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
        tiltValueButton_->setToolTip(
            QStringLiteral("Derived products blend every tilt; no single-tilt view"));
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
        tiltValueButton_->setToolTip(
            QStringLiteral("Pick a radar and load a volume first; tilts list here"));
        map_->setElevationFilter(std::nullopt);
        rebuildTiltGrid();
        return;
    }
    const auto currentChunk =
        frameChunk(*session, static_cast<std::size_t>(session->frameIndex));
    if (!currentChunk) {
        // Spilled frame whose disk copy is gone: same empty treatment as no
        // frame at all rather than dereferencing null.
        tiltCombo_->clear();
        tiltCombo_->addItem(QStringLiteral("Waiting for decoded tilts"));
        tiltCombo_->setEnabled(false);
        tiltValueButton_->setToolTip(
            QStringLiteral("Volume data unavailable; reselect the radar or reload"));
        map_->setElevationFilter(std::nullopt);
        rebuildTiltGrid();
        return;
    }
    const auto& currentVolume = *currentChunk;
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
    tiltValueButton_->setToolTip(!tilts.empty()
                                     ? QStringLiteral("Click for the full tilt list (PgUp/PgDn steps)")
                                     : QStringLiteral("This volume carries no tilts for the product"));
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
        // Faults back from disk on spilled frames; a null chunk (unreadable
        // spill) keeps the panel's current pixels instead of blanking it.
        const auto panelChunk = frameChunk(*panelSession, static_cast<std::size_t>(selected));
        if (!panelChunk) continue;
        const auto& panelFrame = panelSession->history.at(static_cast<std::size_t>(selected));
        // Per-panel availability: in mixed US+EU layouts each panel has its own
        // radar, so a product valid on one panel (e.g. ZDR on NEXRAD) can be
        // missing on another (OPERA single-pol). Fall back per panel instead of
        // leaving it blank; the global chips stay active-panel-scoped.
        const auto panelHas = [&panelChunk](ProductKind kind) {
            return std::any_of(panelChunk->rays.cbegin(), panelChunk->rays.cend(),
                               [kind](const DecodedRay& ray) { return ray.has(kind); });
        };
        if (!panelHas(panel->product())) {
            ProductKind fallback = ProductKind::Reflectivity;
            if (!panelHas(fallback)) {
                bool found = false;
                for (const auto& descriptor : productDescriptors()) {
                    if (panelHas(descriptor.kind)) {
                        fallback = descriptor.kind;
                        found = true;
                        break;
                    }
                }
                if (!found) fallback = panel->product();  // empty volume: keep, don't churn
            }
            if (fallback != panel->product()) {
                panel->setProduct(fallback);
                if (panel == map_ && productCombo_) {
                    const QSignalBlocker comboBlocker(productCombo_);
                    const auto comboIndex = productCombo_->findData(static_cast<int>(fallback));
                    if (comboIndex >= 0) productCombo_->setCurrentIndex(comboIndex);
                }
            }
        }
        panel->setDecodedChunk(panelChunk, panelFrame.dataTime);
        panel->setVolumeQuality(std::max(0, panelFrame.chunkCount - panelFrame.failedChunks),
                                panelFrame.chunkCount, panelFrame.failedChunks,
                                panelChunk->rejectedMessages, panelChunk->rejectedBlocks);
    }
    // Lazy CAPPI refresh: spilled frames faulted back in may carry a field
    // derived at a previous altitude. Re-derive in the background instead
    // of showing a silently wrong height; the swap refreshes the display.
    for (auto* panel : maps_) {
        const auto panelSession = sessionFor(panel->radarId(), false);
        if (!panelSession || panelSession->frameIndex < 0 ||
            panelSession->frameIndex >= static_cast<int>(panelSession->history.size()))
            continue;
        const auto& shown = panelSession->history.at(static_cast<std::size_t>(panelSession->frameIndex));
        if (!shown.chunk) continue;
        const bool hasCappi = std::any_of(
            shown.chunk->rays.cbegin(), shown.chunk->rays.cend(),
            [](const DecodedRay& ray) { return ray.has(ProductKind::Cappi); });
        if (!hasCappi) continue;
        if (!std::isnan(shown.cappiHeightM) && shown.cappiHeightM == cappiHeightM_) continue;
        double radarElevationMeters = 0.0;
        if (model_) {
            const auto radarRow = model_->rowForRadar(panel->radarId());
            if (radarRow >= 0) radarElevationMeters = model_->siteAt(radarRow).antennaElevationMeters;
        }
        launchFrameRederive(panel->radarId(), shown.volumeId, shown.chunk, radarElevationMeters,
                             cappiHeightM_);
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
    refreshCrossSection();
    refreshScanDialog();
    scheduleSpillPrefetch();
}

void MainWindow::updateProductAvailability() {
    auto* itemModel = qobject_cast<QStandardItemModel*>(productCombo_->model());
    if (!itemModel) return;
    const auto session = activeSession();
    const auto hasFrame = session && session->frameIndex >= 0 &&
                          session->frameIndex < static_cast<int>(session->history.size());
    const auto frameChunkForAvailability =
        hasFrame ? frameChunk(*session, static_cast<std::size_t>(session->frameIndex)) : nullptr;
    int firstAvailable = -1;
    for (int row = 0; row < productCombo_->count(); ++row) {
        const auto product = static_cast<ProductKind>(productCombo_->itemData(row).toInt());
        const auto available = !hasFrame || !frameChunkForAvailability ||
                               std::any_of(frameChunkForAvailability->rays.cbegin(),
                                           frameChunkForAvailability->rays.cend(),
                                           [product](const DecodedRay& ray) { return ray.has(product); });
        if (auto* item = itemModel->item(row)) item->setEnabled(available);
        if (available && firstAvailable < 0) firstAvailable = row;
    }
    if (hasFrame && firstAvailable >= 0) {
        const auto currentItem = itemModel->item(productCombo_->currentIndex());
        if (!currentItem || !currentItem->isEnabled()) productCombo_->setCurrentIndex(firstAvailable);
    }
    // The chips are the primary product UI (two-column grid); they mirror the
    // combo model but only refresh on currentIndexChanged. Without this, going
    // OPERA (dual-pol disabled) -> NEXRAD leaves VEL/ZDR/etc chips disabled
    // when the index itself doesn't change (e.g. stays on REF).
    syncProductChips();
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
    refreshTimelineStrip();
}

void MainWindow::stopAnimation() {
    animationTimer_->stop();
    if (playButton_->isChecked()) playButton_->setChecked(false);
    playButton_->setText(QStringLiteral("Play"));
}

// TODO: future extraction point — move QSettings persistence here into a
// SessionStore (load/save session, bookmarks, UI state) without moving code yet.
void MainWindow::restoreSettings() {
    QSettings settings;
    maximumHistoryFrames_ = std::clamp(settings.value(QStringLiteral("timeline/maximumFrames"), 12).toInt(),
                                       2, 32);
    historyVolumeCount_ = std::clamp(settings.value(QStringLiteral("timeline/historyVolumes"), 8).toInt(),
                                    2, 16);
    chaseMode_ = settings.value(QStringLiteral("chase/enabled"), false).toBool();
    chaseLatitude_ = std::clamp(settings.value(QStringLiteral("chase/latitude"), 35.0).toDouble(), -90.0, 90.0);
    chaseLongitude_ = std::clamp(settings.value(QStringLiteral("chase/longitude"), -97.0).toDouble(), -180.0, 180.0);
    synchronizePanels_ = settings.value(QStringLiteral("panels/synchronizeNavigation"), true).toBool();
    theme_ = appThemeFromInt(settings.value(QStringLiteral("ui/theme"), 1).toInt());
    applyTheme(theme_);
    applyFontScale(settings.value(QStringLiteral("ui/fontScale"), 1.0).toDouble());
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
    // Snap a foreign height (older builds, hand edits) to the nearest preset.
    {
        const double saved = settings.value(QStringLiteral("view/cappiHeightM"), 3000.0).toDouble();
        double best = 3000.0;
        double bestDistance = std::abs(saved - best);
        for (const double preset : {2000.0, 3000.0, 4000.0, 5000.0, 7000.0}) {
            const double distance = std::abs(preset - saved);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = preset;
            }
        }
        cappiHeightM_ = best;
    }
    paletteCombo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("view/palette"), 1).toInt(),
                                              0, paletteCombo_->count() - 1));
    refreshPalettePreview();
    radarOpacitySlider_->setValue(std::clamp(settings.value(QStringLiteral("view/radarOpacity"), 80).toInt(),
                                             radarOpacitySlider_->minimum(), radarOpacitySlider_->maximum()));
    warningOpacitySlider_->setValue(std::clamp(settings.value(QStringLiteral("view/warningOpacity"), 90).toInt(),
                                               warningOpacitySlider_->minimum(),
                                               warningOpacitySlider_->maximum()));
    warningsOnTopCheck_->setChecked(settings.value(QStringLiteral("view/warningsOnTop"), true).toBool());
    for (auto* panel : maps_) {
        panel->setWarningOpacity(warningOpacitySlider_->value());
        panel->setWarningsOnTop(warningsOnTopCheck_->isChecked());
    }
    warningFilterCombo_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("overlays/warningFilter"), 0).toInt(),
                                                    0, warningFilterCombo_->count() - 1));
    warningsCheck_->setChecked(settings.value(QStringLiteral("overlays/warnings"), true).toBool());
    reportsCheck_->setChecked(settings.value(QStringLiteral("overlays/reports"), true).toBool());
    tornadoCheck_->setChecked(settings.value(QStringLiteral("overlays/tornado"), true).toBool());
    hailCheck_->setChecked(settings.value(QStringLiteral("overlays/hail"), true).toBool());
    windCheck_->setChecked(settings.value(QStringLiteral("overlays/wind"), true).toBool());
    belowThresholdCheck_->setChecked(settings.value(QStringLiteral("view/belowThreshold"), false).toBool());
    trailCheck_->setChecked(settings.value(QStringLiteral("overlays/trail"), true).toBool());
    smoothingCheck_->setChecked(settings.value(QStringLiteral("view/smoothing"), false).toBool());
    shearCheck_->setChecked(settings.value(QStringLiteral("overlays/shear"), true).toBool());
    tvsCheck_->setChecked(settings.value(QStringLiteral("overlays/tvs"), true).toBool());
    hailCoreCheck_->setChecked(settings.value(QStringLiteral("overlays/hailCores"), true).toBool());
    zdrColumnCheck_->setChecked(settings.value(QStringLiteral("overlays/zdrColumns"), true).toBool());
    tdsCheck_->setChecked(settings.value(QStringLiteral("overlays/tds"), true).toBool());
    for (auto* panel : maps_) {
        panel->setStormTrailVisible(trailCheck_->isChecked());
        panel->setDisplaySmoothing(smoothingCheck_->isChecked());
        panel->setShearVisible(shearCheck_->isChecked());
        panel->setTvsVisible(tvsCheck_->isChecked());
        panel->setHailVisible(hailCoreCheck_->isChecked());
        panel->setZdrVisible(zdrColumnCheck_->isChecked());
        panel->setTdsVisible(tdsCheck_->isChecked());
        panel->setSpcOutlookVisible(spcOutlookCheck_->isChecked());
        panel->setMcdVisible(mcdCheck_->isChecked());
        panel->setMetarVisible(metarCheck_->isChecked());
    }
    if (lightningCheck_->isEnabled())
        lightningCheck_->setChecked(settings.value(QStringLiteral("overlays/lightning"), true).toBool());
    if (lightningFusionCheck_->isEnabled())
        lightningFusionCheck_->setChecked(settings.value(QStringLiteral("overlays/lightningFusion"), true).toBool());
    placefilesCheck_->setChecked(settings.value(QStringLiteral("overlays/placefiles"), true).toBool());
    spcOutlookCheck_->setChecked(settings.value(QStringLiteral("overlays/spcOutlook"), true).toBool());
    mcdCheck_->setChecked(settings.value(QStringLiteral("overlays/mcd"), true).toBool());
    metarCheck_->setChecked(settings.value(QStringLiteral("overlays/metar"), true).toBool());
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
    if (sidebarTabs_)
        sidebarTabs_->setCurrentIndex(std::clamp(settings.value(QStringLiteral("ui/sidebarTab"), 0).toInt(),
                                                 0, sidebarTabs_->count() - 1));
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
    settings.setValue(QStringLiteral("view/cappiHeightM"), cappiHeightM_);
    settings.setValue(QStringLiteral("view/palette"), paletteCombo_->currentIndex());
    QStringList customStops;
    for (const auto& stop : map_->customPalette())
        customStops.push_back(QStringLiteral("%1,%2").arg(stop.value, 0, 'g', 12).arg(stop.color.name(QColor::HexArgb)));
    settings.setValue(QStringLiteral("view/customPalette"), customStops);
    settings.setValue(QStringLiteral("view/radarOpacity"), radarOpacitySlider_->value());
    settings.setValue(QStringLiteral("view/warningOpacity"), warningOpacitySlider_->value());
    settings.setValue(QStringLiteral("view/warningsOnTop"), warningsOnTopCheck_->isChecked());
    settings.setValue(QStringLiteral("view/belowThreshold"), belowThresholdCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/trail"), trailCheck_->isChecked());
    settings.setValue(QStringLiteral("view/smoothing"), smoothingCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/shear"), shearCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/tvs"), tvsCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/hailCores"), hailCoreCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/zdrColumns"), zdrColumnCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/tds"), tdsCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/warnings"), warningsCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/reports"), reportsCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/tornado"), tornadoCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/hail"), hailCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/wind"), windCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/warningFilter"), warningFilterCombo_->currentIndex());
    settings.setValue(QStringLiteral("overlays/lightning"), lightningCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/lightningFusion"), lightningFusionCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/placefiles"), placefilesCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/spcOutlook"), spcOutlookCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/mcd"), mcdCheck_->isChecked());
    settings.setValue(QStringLiteral("overlays/metar"), metarCheck_->isChecked());
    settings.setValue(QStringLiteral("timeline/speed"), speedCombo_->currentIndex());
    settings.setValue(QStringLiteral("timeline/maximumFrames"), maximumHistoryFrames_);
    settings.setValue(QStringLiteral("timeline/historyVolumes"), historyVolumeCount_);
    settings.setValue(QStringLiteral("chase/enabled"), chaseMode_);
    settings.setValue(QStringLiteral("chase/latitude"), chaseLatitude_);
    settings.setValue(QStringLiteral("chase/longitude"), chaseLongitude_);
    settings.setValue(QStringLiteral("panels/layout"), panelLayoutCombo_->currentIndex());
        settings.setValue(QStringLiteral("panels/synchronizeNavigation"), synchronizePanels_);
        settings.setValue(QStringLiteral("ui/theme"), static_cast<int>(theme_));
        settings.setValue(QStringLiteral("ui/fontScale"), fontScale_);
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
    // 23 form rows plus the alerts group box overflowed a short dialog and
    // pushed OK/Cancel off-screen at any non-default text size. Put the body
    // in a scroll area and cap the height so the button box always stays put.
    auto* settingsScroll = new QScrollArea(&dialog);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    auto* settingsBody = new QWidget(settingsScroll);
    auto* bodyLayout = new QVBoxLayout(settingsBody);
    bodyLayout->setContentsMargins(0, 0, 8, 0);
    // Grouped sections instead of one 23-row flat form: the flat list made
    // related knobs (all six GLM rows, all seven network rows) blur together.
    const auto makeSection = [&dialog, bodyLayout](const QString& title) {
        auto* group = new QGroupBox(title, &dialog);
        auto* sectionForm = new QFormLayout(group);
        bodyLayout->addWidget(group);
        return sectionForm;
    };
    auto* displayForm = makeSection(QStringLiteral("Display"));
    auto* chaseForm = makeSection(QStringLiteral("Chase"));
    auto* timelineForm = makeSection(QStringLiteral("Timeline"));
    auto* networkForm = makeSection(QStringLiteral("Network"));
    auto* glmForm = makeSection(QStringLiteral("Lightning (GLM)"));
    auto* cacheForm = makeSection(QStringLiteral("Map cache"));
    auto* basemapForm = makeSection(QStringLiteral("Basemap"));
    auto* themeCombo = new QComboBox(&dialog);
    themeCombo->addItem(appThemeName(AppTheme::Classic), static_cast<int>(AppTheme::Classic));
    themeCombo->addItem(appThemeName(AppTheme::Prototype), static_cast<int>(AppTheme::Prototype));
    themeCombo->setCurrentIndex(themeCombo->findData(static_cast<int>(theme_)));
    themeCombo->setToolTip(QStringLiteral("Switch the application identity live. Squall is the redesign prototype."));
    auto* fontScale = new QSpinBox(&dialog);
    fontScale->setRange(80, 150);
    fontScale->setSuffix(QStringLiteral(" %"));
    fontScale->setSingleStep(5);
    fontScale->setValue(static_cast<int>(std::round(fontScale_ * 100.0)));
    fontScale->setToolTip(QStringLiteral(
        "Scales map HUD text and UI type live, no restart. Theme-pinned chrome (dock titles, badges, tabs) stays fixed."));
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
    frames->setRange(2, 32);
    frames->setValue(maximumHistoryFrames_);
    frames->setToolTip(QStringLiteral("Volumes kept in memory for looping. Above ~24 uses significant RAM."));
    auto* volumes = new QSpinBox(&dialog);
    volumes->setRange(2, 16);
    volumes->setValue(historyVolumeCount_);
    volumes->setToolTip(QStringLiteral("How many past volumes to fetch for the timeline."));
    auto* contact = new QLineEdit(QSettings().value(QStringLiteral("network/contact")).toString(), &dialog);
    contact->setPlaceholderText(QStringLiteral("Email or project URL for public-service requests"));
    auto* synchronize = new QCheckBox(QStringLiteral("Link pan and zoom across radar panels"), &dialog);
    synchronize->setChecked(synchronizePanels_);
    auto* parallelChunks = new QSpinBox(&dialog);
    parallelChunks->setRange(1, 16);
    parallelChunks->setValue(QSettings().value(QStringLiteral("network/parallelChunks"), 10).toInt());
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
    // Basemap provider + API key. The key lives in the user's own settings
    // (registry/plist/ini), never in the install dir, so updates keep it.
    auto* basemapProvider = new QComboBox(&dialog);
    basemapProvider->addItem(basemapProviderName(BasemapProvider::CartoDark),
                             static_cast<int>(BasemapProvider::CartoDark));
    basemapProvider->addItem(basemapProviderName(BasemapProvider::CartoLight),
                             static_cast<int>(BasemapProvider::CartoLight));
    basemapProvider->addItem(basemapProviderName(BasemapProvider::EsriDark),
                             static_cast<int>(BasemapProvider::EsriDark));
    basemapProvider->addItem(basemapProviderName(BasemapProvider::Custom),
                             static_cast<int>(BasemapProvider::Custom));
    basemapProvider->setCurrentIndex(
        basemapProvider->findData(QSettings().value(QStringLiteral("basemap/provider"), 0).toInt()));
    basemapProvider->setToolTip(QStringLiteral(
        "CARTO serves the map from your own free key (no shared keyless endpoint). "
        "Esri Dark Gray is the pre-CARTO fallback."));
    auto* basemapKey = new QLineEdit(&dialog);
    basemapKey->setEchoMode(QLineEdit::Password);
    basemapKey->setPlaceholderText(QStringLiteral("Free key from carto.com/basemaps/apikey"));
    basemapKey->setText(QSettings().value(QStringLiteral("basemap/apiKey")).toString());
    basemapKey->setToolTip(QStringLiteral(
        "Stored in your user settings, so app updates never ask for it again. "
        "Sent only to the tile endpoint as ?key=. Without a key the map "
        "uses Esri instead of watermarked CARTO tiles."));
    auto* basemapTemplate = new QLineEdit(&dialog);
    basemapTemplate->setPlaceholderText(
        QStringLiteral("https://tiles.example.com/{z}/{x}/{y}.png?key={key}"));
    basemapTemplate->setText(QSettings().value(QStringLiteral("basemap/customTemplate")).toString());
    auto* basemapCredit = new QLineEdit(&dialog);
    basemapCredit->setPlaceholderText(QStringLiteral("(c) OpenStreetMap contributors"));
    basemapCredit->setText(QSettings().value(QStringLiteral("basemap/customAttribution")).toString());
    const auto syncBasemapRows = [basemapProvider, basemapTemplate, basemapCredit] {
        const bool custom = basemapProvider->currentData().toInt() ==
                            static_cast<int>(BasemapProvider::Custom);
        basemapTemplate->setEnabled(custom);
        basemapCredit->setEnabled(custom);
    };
    connect(basemapProvider, &QComboBox::currentIndexChanged, &dialog,
            [syncBasemapRows](int) { syncBasemapRows(); });
    syncBasemapRows();
    displayForm->addRow(QStringLiteral("Theme"), themeCombo);
    displayForm->addRow(QStringLiteral("Interface text scale"), fontScale);
    displayForm->addRow(QStringLiteral("Multi-panel"), synchronize);
    chaseForm->addRow(QStringLiteral("Chase mode"), chaseCheck);
    chaseForm->addRow(QStringLiteral("Chase latitude"), latitude);
    chaseForm->addRow(QStringLiteral("Chase longitude"), longitude);
    chaseForm->addRow(QString(), chaseRow);
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
    timelineForm->addRow(QStringLiteral("Frames kept in memory"), frames);
    timelineForm->addRow(QStringLiteral("Volumes loaded for playback"), volumes);
    frames->setToolTip(QStringLiteral("Applies immediately; excess frames spill to disk."));
    volumes->setToolTip(QStringLiteral("Applies immediately to the next history load."));
    networkForm->addRow(QStringLiteral("Network contact"), contact);
    networkForm->addRow(QStringLiteral("Parallel radar chunks"), parallelChunks);
    networkForm->addRow(QStringLiteral("Request timeout"), timeout);
    networkForm->addRow(QStringLiteral("Chunk retries"), retries);
    networkForm->addRow(QStringLiteral("Initial retry backoff"), backoff);
    networkForm->addRow(QStringLiteral("Radar refresh"), radarRefresh);
    networkForm->addRow(QStringLiteral("Warning/report refresh"), overlayRefresh);
    contact->setToolTip(QStringLiteral("Applies immediately to new requests."));
    parallelChunks->setToolTip(QStringLiteral("Applies immediately to new requests."));
    timeout->setToolTip(QStringLiteral("Applies immediately to new requests."));
    retries->setToolTip(QStringLiteral("Applies immediately to new requests."));
    backoff->setToolTip(QStringLiteral("Applies immediately to new requests."));
    radarRefresh->setToolTip(QStringLiteral("Applies immediately; restarts the refresh timer."));
    overlayRefresh->setToolTip(QStringLiteral("Applies immediately; restarts the refresh timer."));
    glmForm->addRow(QStringLiteral("GLM refresh"), lightningRefresh);
    glmForm->addRow(QStringLiteral("GLM initial history"), lightningBackfill);
    glmForm->addRow(QStringLiteral("GLM cell size"), lightningCell);
    glmForm->addRow(QStringLiteral("GLM minimum rate"), lightningRate);
    glmForm->addRow(QStringLiteral("GLM jump threshold"), lightningSigma);
    glmForm->addRow(QStringLiteral("GLM sigma window"), lightningWindow);
    cacheForm->addRow(QStringLiteral("Map memory cache (restart)"), memoryCache);
    cacheForm->addRow(QStringLiteral("Map disk cache (restart)"), diskCache);
    basemapForm->addRow(QStringLiteral("Provider"), basemapProvider);
    basemapForm->addRow(QStringLiteral("CARTO API key"), basemapKey);
    basemapForm->addRow(QStringLiteral("Custom template"), basemapTemplate);
    basemapForm->addRow(QStringLiteral("Custom attribution"), basemapCredit);
    memoryCache->setToolTip(QStringLiteral("Requires restart: the map tile store reads this once at first use."));
    diskCache->setToolTip(QStringLiteral("Requires restart: the map tile store reads this once at first use."));

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
    alertsLayout->addRow(QStringLiteral("Event filter list"), alertsEventList);
    alertsLayout->addRow(QStringLiteral("Lightning jump threshold"), alertsSigma);
    alertsLayout->addRow(QStringLiteral("Suppress repeats for"), alertsSuppression);
    alertsLayout->addRow(QStringLiteral("Custom sound (.wav)"), alertsSoundContainer);
    alertsLayout->addRow(alertsTestButton);
    bodyLayout->addWidget(alertsGroup);

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
    bodyLayout->addWidget(alertNote);
    bodyLayout->addStretch(1);
    settingsScroll->setWidget(settingsBody);
    layout->addWidget(settingsScroll, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    // The scroll-area body reports a tiny sizeHint by design (it scrolls, so
    // it never asks to fit its content). Sizing from dialog.height() therefore
    // collapses to a postage stamp. Size from the screen instead, same pattern
    // as the main-window default, with a minimum so OK/Cancel stay reachable.
    if (const auto* screen = QGuiApplication::primaryScreen()) {
        const auto available = screen->availableGeometry();
        dialog.resize(560, std::clamp(available.height() - 120, 480, 680));
    } else {
        dialog.resize(560, 680);
    }
    dialog.setMinimumSize(480, 400);
    if (dialog.exec() != QDialog::Accepted) return;

    chaseMode_ = chaseCheck->isChecked();
    applyTheme(appThemeFromInt(themeCombo->currentData().toInt()));
    applyFontScale(fontScale->value() / 100.0);
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
    const auto oldBasemapProvider = networkSettings.value(QStringLiteral("basemap/provider"), 0).toInt();
    const auto oldBasemapKey = networkSettings.value(QStringLiteral("basemap/apiKey")).toString();
    const auto oldBasemapTemplate =
        networkSettings.value(QStringLiteral("basemap/customTemplate")).toString();
    const auto oldBasemapCredit =
        networkSettings.value(QStringLiteral("basemap/customAttribution")).toString();
    networkSettings.setValue(QStringLiteral("basemap/provider"), basemapProvider->currentData().toInt());
    networkSettings.setValue(QStringLiteral("basemap/apiKey"), basemapKey->text().trimmed());
    networkSettings.setValue(QStringLiteral("basemap/customTemplate"),
                             basemapTemplate->text().trimmed());
    networkSettings.setValue(QStringLiteral("basemap/customAttribution"),
                             basemapCredit->text().trimmed());
    if (basemapProvider->currentData().toInt() != oldBasemapProvider ||
        basemapKey->text().trimmed() != oldBasemapKey ||
        basemapTemplate->text().trimmed() != oldBasemapTemplate ||
        basemapCredit->text().trimmed() != oldBasemapCredit) {
        // New endpoint or key: drop cached tiles so every panel refetches
        // from it instead of showing the old provider's layers.
        RadarMapCanvas::clearSharedTileCache();
        for (auto* map : maps_)
            if (map) map->update();
    }
    refreshTimer_->setInterval(radarRefresh->value() * 1000);
    overlayRefreshTimer_->setInterval(overlayRefresh->value() * 1000);
    glmSource_->configure(glmConfigFromSettings());
    glmSource_->refresh();
    for (const auto& session : radarSessions_) enforceSpillBudget(*session);
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
    if (panelMaximized_) {
        // A layout change exits temporary maximize; the preset combo owns truth.
        panelMaximized_ = false;
        if (maximizePanelButton_) {
            const QSignalBlocker blocker(maximizePanelButton_);
            maximizePanelButton_->setChecked(false);
        }
    }
    layoutBeforeMaximize_ = layoutIndex;
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

void MainWindow::togglePanelMaximized() {
    if (!mapGrid_ || maps_.isEmpty() || !map_) return;
    const auto activeIndex = std::max(0, panelIndex(map_));
    if (!panelMaximized_) {
        maximizedPanelIndex_ = activeIndex;
        if (panelLayoutCombo_) layoutBeforeMaximize_ = panelLayoutCombo_->currentIndex();
        panelMaximized_ = true;
        for (auto* panel : maps_) {
            mapGrid_->removeWidget(panel);
            panel->hide();
        }
        mapGrid_->addWidget(maps_.at(maximizedPanelIndex_), 0, 0, 2, 2);
        maps_.at(maximizedPanelIndex_)->show();
        mapGrid_->setRowStretch(0, 1);
        mapGrid_->setRowStretch(1, 1);
        mapGrid_->setColumnStretch(0, 1);
        mapGrid_->setColumnStretch(1, 0);
        statusBar()->showMessage(
            QStringLiteral("Panel %1 maximized — toggle off to restore %2")
                .arg(maximizedPanelIndex_ + 1)
                .arg(panelLayoutCombo_ ? panelLayoutCombo_->currentText() : QStringLiteral("layout")),
            4000);
    } else {
        panelMaximized_ = false;
        if (maximizePanelButton_) {
            const QSignalBlocker blocker(maximizePanelButton_);
            maximizePanelButton_->setChecked(false);
        }
        // Restore via the normal layout path (which also re-saves settings).
        const auto restore = layoutBeforeMaximize_;
        if (panelLayoutCombo_) {
            const QSignalBlocker blocker(panelLayoutCombo_);
            panelLayoutCombo_->setCurrentIndex(restore);
        }
        // Manually re-apply since we blocked the combo signal.
        panelMaximized_ = false; // setPanelLayout clears it again; keep consistent
        // Temporarily pretend we are not maximized so setPanelLayout takes the normal path.
        setPanelLayout(restore);
        if (maximizePanelButton_) {
            const QSignalBlocker blocker(maximizePanelButton_);
            maximizePanelButton_->setChecked(false);
        }
        activateMap(maps_.at(maximizedPanelIndex_));
    }
}

void MainWindow::updateOfflineBanner() {
    if (!offlineBanner_) return;
    const bool show = consecutiveFailures_ > 0;
    offlineBanner_->setVisible(show);
    if (show) {
        const auto* parent = offlineBanner_->parentWidget();
        if (parent) {
            const auto bannerWidth = std::min(520, parent->width() - 40);
            offlineBanner_->setGeometry(
                (parent->width() - bannerWidth) / 2, 10,
                bannerWidth, offlineBanner_->sizeHint().height() + 8);
            offlineBanner_->raise();
        }
        if (emptyStateHint_) emptyStateHint_->hide();
    }
}

void MainWindow::applyDataSaverPreset() {
    QSettings settings;
    settings.setValue(QStringLiteral("network/parallelChunks"), 2);
    settings.setValue(QStringLiteral("timeline/historyVolumes"), 4);
    settings.setValue(QStringLiteral("network/overlayRefreshSeconds"), 120);
    historyVolumeCount_ = 4;
    overlayRefreshTimer_->start(120 * 1000);
    statusBar()->showMessage(
        QStringLiteral("Data-saver on: 2 parallel chunks, 4-frame history, overlays every 2 min"), 6000);
}

void MainWindow::setupMenus() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* snapshotAction = fileMenu->addAction(QStringLiteral("Export snapshot…"));
    snapshotAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    connect(snapshotAction, &QAction::triggered, this, &MainWindow::exportSnapshot);
    auto* gifAction = fileMenu->addAction(QStringLiteral("Export timeline GIF…"));
    connect(gifAction, &QAction::triggered, this, &MainWindow::exportTimelineAnimation);
    auto* mp4Action = fileMenu->addAction(QStringLiteral("Record timeline MP4…"));
    connect(mp4Action, &QAction::triggered, this, &MainWindow::exportTimelineMp4);
    auto* webmAction = fileMenu->addAction(QStringLiteral("Export timeline WebM…"));
    connect(webmAction, &QAction::triggered, this, &MainWindow::exportTimelineWebM);
    auto* kmlAction = fileMenu->addAction(QStringLiteral("Export frame KML (GIS)…"));
    connect(kmlAction, &QAction::triggered, this, &MainWindow::exportFrameKml);
    auto* caseExportAction = fileMenu->addAction(QStringLiteral("Export case-pack…"));
    connect(caseExportAction, &QAction::triggered, this, &MainWindow::exportCasePack);
    auto* caseImportAction = fileMenu->addAction(QStringLiteral("Import case-pack…"));
    connect(caseImportAction, &QAction::triggered, this, &MainWindow::importCasePack);
    auto* packAction = fileMenu->addAction(QStringLiteral("Download offline map pack…"));
    connect(packAction, &QAction::triggered, this, &MainWindow::downloadOfflinePack);
    fileMenu->addSeparator();
    auto* settingsAction = fileMenu->addAction(QStringLiteral("Settings…"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettingsDialog);
    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    auto* controlAct = viewMenu->addAction(QStringLiteral("Radar controls dock"));
    controlAct->setCheckable(true);
    controlAct->setChecked(controlDock_ && controlDock_->isVisible());
    connect(controlAct, &QAction::toggled, this, [this](bool visible) {
        if (controlDock_) controlDock_->setVisible(visible);
    });
    connect(controlDock_, &QDockWidget::visibilityChanged, controlAct, &QAction::setChecked);
    auto* playbackAct = viewMenu->addAction(QStringLiteral("Playback dock"));
    playbackAct->setCheckable(true);
    playbackAct->setChecked(playbackDock_ && playbackDock_->isVisible());
    connect(playbackAct, &QAction::toggled, this, [this](bool visible) {
        if (playbackDock_) playbackDock_->setVisible(visible);
    });
    connect(playbackDock_, &QDockWidget::visibilityChanged, playbackAct, &QAction::setChecked);
    viewMenu->addSeparator();
    auto* maxAct = viewMenu->addAction(QStringLiteral("Maximize active panel"));
    maxAct->setShortcut(QKeySequence(Qt::Key_F11));
    maxAct->setCheckable(true);
    connect(maxAct, &QAction::toggled, this, [this](bool checked) {
        if (maximizePanelButton_) maximizePanelButton_->setChecked(checked);
        else togglePanelMaximized();
    });
    connect(this, &MainWindow::destroyed, maxAct, &QObject::deleteLater);
    auto* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    auto* paletteAction = helpMenu->addAction(QStringLiteral("Command palette…"));
    paletteAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+K")));
    connect(paletteAction, &QAction::triggered, this, &MainWindow::showCommandPalette);
    auto* shortcutsAction = helpMenu->addAction(QStringLiteral("Keyboard shortcuts…"));
    shortcutsAction->setShortcuts({QKeySequence(Qt::Key_F1), QKeySequence(QStringLiteral("?"))});
    connect(shortcutsAction, &QAction::triggered, this, [this] {
        auto* dialog = new ShortcutHelpDialog(this);
        dialog->setModal(false);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });
    auto* aboutAction = helpMenu->addAction(QStringLiteral("About CappedWeather…"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAboutDialog);
}

void MainWindow::showCommandPalette() {
    auto* palette = new CommandPalette(this);
    QVector<CommandPalette::Entry> entries;
    auto actions = std::make_shared<std::vector<std::function<void()>>>();
    const auto addEntry = [&entries, &actions](QString title, QString detail,
                                               std::function<void()> action) {
        entries.push_back({std::move(title), std::move(detail)});
        actions->push_back(std::move(action));
    };
    // Radars first: the most common palette trip is jumping stations.
    if (model_) {
        for (const auto& site : model_->sites()) {
            const auto id = QString::fromStdString(site.id);
            const auto name = QString::fromStdString(site.name);
            addEntry(QStringLiteral("Radar %1").arg(id),
                     name.isEmpty() ? QStringLiteral("Jump to radar")
                                    : QStringLiteral("Jump to radar (%1)").arg(name),
                     [this, id] {
                         const auto row = model_ ? model_->rowForRadar(id) : -1;
                         if (row >= 0) radarSelected(model_->index(row, 0));
                     });
        }
    }
    for (int product = 0; product < productCombo_->count(); ++product) {
        const auto kind =
            static_cast<ProductKind>(productCombo_->itemData(product).toInt());
        addEntry(QStringLiteral("Product: %1").arg(productCombo_->itemText(product)),
                 QStringLiteral("%1 — switch the active panel").arg(productShortBlurb(kind)),
                 [this, product] { productCombo_->setCurrentIndex(product); });
    }
    for (int layout = 0; layout < panelLayoutCombo_->count(); ++layout) {
        addEntry(QStringLiteral("Layout: %1").arg(panelLayoutCombo_->itemText(layout)),
                 QStringLiteral("Rearrange panels"),
                 [this, layout] { setPanelLayout(layout); });
    }
    addEntry(QStringLiteral("Go: national view"), QStringLiteral("Zoom out to CONUS"),
             [this] { map_->showNationalView(); });
    addEntry(QStringLiteral("Go: recenter on radar"), QStringLiteral("Shortcut R"),
             [this] { map_->recenterSelected(); });
    addEntry(QStringLiteral("Timeline: play / pause"), QStringLiteral("Shortcut Space"),
             [this] { playButton_->toggle(); });
    addEntry(QStringLiteral("Timeline: follow LIVE"), QStringLiteral("Shortcut L"),
             [this] { liveButton_->setChecked(true); });
    addEntry(QStringLiteral("Timeline: bookmark this frame"), QStringLiteral("Shortcut B"),
             [this] { toggleFrameBookmark(); });
    addEntry(QStringLiteral("View: maximize active panel"), QStringLiteral("Shortcut F11"),
             [this] {
                 if (maximizePanelButton_) maximizePanelButton_->toggle();
                 else togglePanelMaximized();
             });
    addEntry(QStringLiteral("View: data-saver preset"), QStringLiteral("Flaky-cellular chase mode"),
             [this] { applyDataSaverPreset(); });
    addEntry(QStringLiteral("Export: snapshot PNG"), QStringLiteral("Shortcut Ctrl+S"),
             [this] { exportSnapshot(); });
    addEntry(QStringLiteral("Export: timeline GIF"), QStringLiteral("Cancellable, with progress"),
             [this] { exportTimelineAnimation(); });
    addEntry(QStringLiteral("Export: timeline MP4"), QStringLiteral("Realtime recording, sharing-friendly"),
             [this] { exportTimelineMp4(); });
    addEntry(QStringLiteral("Export: timeline WebM"), QStringLiteral("Frame-exact VP9 via ffmpeg"),
             [this] { exportTimelineWebM(); });
    addEntry(QStringLiteral("Export: frame KML"), QStringLiteral("PNG plus GroundOverlay for GIS"),
             [this] { exportFrameKml(); });
    addEntry(QStringLiteral("Export: case-pack"), QStringLiteral("Share this case as one file"),
             [this] { exportCasePack(); });
    addEntry(QStringLiteral("Import: case-pack"), QStringLiteral("Reproduce a shared case exactly"),
             [this] { importCasePack(); });
    addEntry(QStringLiteral("Offline: download map pack"), QStringLiteral("Cache basemap for no-signal chasing"),
             [this] { downloadOfflinePack(); });
    addEntry(QStringLiteral("Open: settings"), QStringLiteral("Network, chase, alerts, history"),
             [this] { showSettingsDialog(); });
    palette->setEntries(std::move(entries));
    connect(palette, &CommandPalette::entryChosen, this,
            [actions](int index) {
                if (index >= 0 && index < static_cast<int>(actions->size())) (*actions)[index]();
            });
    palette->show();
}

void MainWindow::showAboutDialog() {
    QMessageBox::about(
        this, QStringLiteral("About CappedWeather"),
        QStringLiteral("CappedWeather %1\n"
                       "Copyright (C) 2026 Mikifrfr\n\n"
                       "This program is free software: you can redistribute it and/or modify "
                       "it under the terms of the GNU Affero General Public License as published by "
                       "the Free Software Foundation, either version 3 of the License, or "
                       "(at your option) any later version.\n\n"
                       "This program is distributed in the hope that it will be useful, "
                       "but WITHOUT ANY WARRANTY.\n\n"
                       "Source: https://github.com/Mikifrfr/CappedWeather\n"
                       "License: see LICENSE file.")
            .arg(QStringLiteral(CAPPEDWEATHER_VERSION)));
}

void MainWindow::activateMap(RadarMapCanvas* map) {
    if (!map || map_ == map && map->property("activePanel").toBool()) return;
    // Preserve the outgoing tool state so switching panels doesn't silently
    // cancel a measure / cross-section gesture. The tool transfers to the new
    // active panel instead of vanishing.
    const bool hadMeasure = map_ && measureButton_ && measureButton_->isChecked();
    const bool hadCrossSection = map_ && crossSectionButton_ && crossSectionButton_->isChecked();
    if (map_) {
        map_->setMeasureMode(false);
        map_->setCrossSectionMode(false);
    }
    map_ = map;
    for (auto* panel : maps_) {
        panel->setActivePanel(panel == map_);
        panel->setProperty("activePanel", panel == map_);
        if (panel != map_) {
            panel->setMeasureMode(false);
            panel->setCrossSectionMode(false);
        }
    }
    if (hadMeasure) map_->setMeasureMode(true);
    if (hadCrossSection && !isDerivedProduct(map_->product())) map_->setCrossSectionMode(true);
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
    measureButton_->setChecked(hadMeasure);
    crossSectionButton_->setChecked(hadCrossSection && !isDerivedProduct(map_->product()));
    updateCrossSectionAvailability();
    selectedRadarId_ = map_->radarId();
    rebuildStationPicker();
    updateStationSummary();
    updateProductAvailability();
    rebuildTiltCombo();
    updateVelocityControls();
    refreshCappiBox();
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
        const bool enabled = !item || item->isEnabled();
        productChips_.at(i)->setEnabled(enabled);
        // A greyed chip without a reason reads as broken. Derived moments
        // need a multi-tilt volume (still deriving in background); native
        // moments mean the site simply has no such data (OPERA single-pol).
        auto tip = productChipTooltip(static_cast<ProductKind>(
            productCombo_->itemData(i).toInt()));
        if (!enabled) {
            const auto kind =
                static_cast<ProductKind>(productCombo_->itemData(i).toInt());
            tip += isDerivedProduct(kind)
                       ? QStringLiteral("\nNot in this volume yet: needs a complete "
                                        "multi-tilt scan (derives in background).")
                       : QStringLiteral("\nNot in this volume: the site sent no such moment.");
        }
        productChips_.at(i)->setToolTip(tip);
    }
    if (activeProductLabel_ && current >= 0 && current < productCombo_->count()) {
        const auto& descriptor = productDescriptors()[static_cast<std::size_t>(current)];
        activeProductLabel_->setText(
            QStringLiteral("%1  ·  %2 %3")
                .arg(QString::fromUtf8(descriptor.label.data()),
                     QString::fromUtf8(descriptor.shortName.data()),
                     QString::fromUtf8(descriptor.unit.data())));
        activeProductLabel_->setToolTip(wrapTooltipText(productExplainer(descriptor.kind)));
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
    if (spcOutlookBadge_) {
        const auto n = map_->spcOutlookCount();
        const bool on = spcOutlookCheck_ && spcOutlookCheck_->isChecked();
        spcOutlookBadge_->setText(!on ? QStringLiteral("off") : n > 0 ? QString::number(n)
                                                                      : QStringLiteral("quiet"));
        spcOutlookBadge_->setToolTip(!on ? QStringLiteral("SPC outlook layer is off")
                                     : n > 0 ? QStringLiteral("Day 1 + 2 outlook areas on the map")
                                             : QStringLiteral("Layer is on; no outlook areas issued right now"));
    }
    if (mcdBadge_) {
        const auto n = map_->mcdCount();
        const bool on = mcdCheck_ && mcdCheck_->isChecked();
        mcdBadge_->setText(!on ? QStringLiteral("off") : n > 0 ? QString::number(n)
                                                               : QStringLiteral("quiet"));
        mcdBadge_->setToolTip(!on ? QStringLiteral("MCD layer is off")
                              : n > 0 ? QStringLiteral("Active mesoscale discussions on the map")
                                      : QStringLiteral("Layer is on; no MCDs in effect right now"));
    }
    if (metarBadge_) {
        const auto n = map_->metarCount();
        const bool on = metarCheck_ && metarCheck_->isChecked();
        metarBadge_->setText(!on ? QStringLiteral("off") : n > 0 ? QString::number(n)
                                                                 : QStringLiteral("quiet"));
        metarBadge_->setToolTip(!on ? QStringLiteral("METAR layer is off")
                                : n > 0 ? QStringLiteral("Surface stations on the map")
                                        : QStringLiteral("Layer is on; no stations reporting in this region right now"));
    }
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
    if (shearBadge_) {
        const auto n = map_->radarFeatureCount();
        shearBadge_->setText(!shearCheck_ || !shearCheck_->isChecked() ? QStringLiteral("off")
                            : n > 0 ? QString::number(n) : QStringLiteral("idle"));
    }
    if (tvsBadge_) {
        const auto n = map_->circulationCount();
        tvsBadge_->setText(!tvsCheck_ || !tvsCheck_->isChecked() ? QStringLiteral("off")
                          : n > 0 ? QString::number(n) : QStringLiteral("idle"));
    }
    if (hailCoreBadge_) {
        const auto n = map_->hailCoreCount();
        hailCoreBadge_->setText(!hailCoreCheck_ || !hailCoreCheck_->isChecked() ? QStringLiteral("off")
                                : n > 0 ? QString::number(n) : QStringLiteral("idle"));
    }
    if (zdrColumnBadge_) {
        const auto n = map_->zdrColumnCount();
        zdrColumnBadge_->setText(!zdrColumnCheck_ || !zdrColumnCheck_->isChecked() ? QStringLiteral("off")
                                 : n > 0 ? QString::number(n) : QStringLiteral("idle"));
    }
    if (tdsBadge_) {
        const auto n = map_->tdsCount();
        tdsBadge_->setText(!tdsCheck_ || !tdsCheck_->isChecked() ? QStringLiteral("off")
                          : n > 0 ? QString::number(n) : QStringLiteral("idle"));
    }
    if (scanDiagnosticsLabel_ && map_) {
        const auto text = map_->scanDiagnosticsText();
        scanDiagnosticsLabel_->setText(text.isEmpty() ? QStringLiteral("Scan: —")
                                                      : QStringLiteral("Scan: %1").arg(text));
    }
}

void MainWindow::refreshWarningList() {
    if (!warningList_) return;
    // Same order the canvas paints: severity priority first, then newest.
    // Indexes stored in UserRole address latestWarnings_ (and therefore the
    // canvases, which hold the same vector in the same order).
    QVector<int> order;
    order.reserve(latestWarnings_.size());
    QString filter;
    if (warningFilterCombo_ && warningFilterCombo_->currentIndex() > 0)
        filter = warningFilterCombo_->itemText(warningFilterCombo_->currentIndex());
    for (int i = 0; i < latestWarnings_.size(); ++i) {
        const auto& w = latestWarnings_.at(i);
        if (!filter.isEmpty() && !w.event.contains(filter, Qt::CaseInsensitive) &&
            !w.severity.contains(filter, Qt::CaseInsensitive))
            continue;
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [this](int left, int right) {
        const auto& l = latestWarnings_.at(left);
        const auto& r = latestWarnings_.at(right);
        if (l.alertPriority != r.alertPriority) return l.alertPriority < r.alertPriority;
        return l.onset > r.onset;
    });
    warningList_->setUpdatesEnabled(false);
    warningList_->clear();
    constexpr int kInboxCap = 200;
    for (int n = 0; n < order.size() && n < kInboxCap; ++n) {
        const auto& w = latestWarnings_.at(order.at(n));
        const auto when = w.onset.isValid() ? w.onset.toLocalTime().toString(QStringLiteral("MMM d HH:mm"))
                                            : QStringLiteral("—");
        auto* item = new QListWidgetItem(
            QStringLiteral("%1 — %2 (%3)").arg(w.event, w.headline.isEmpty() ? w.severity : w.headline, when),
            warningList_);
        item->setData(Qt::UserRole, order.at(n));
        item->setToolTip(QStringLiteral("%1\n%2\nValid: %3 – %4")
                             .arg(w.event, w.headline,
                                  w.onset.isValid() ? w.onset.toLocalTime().toString(Qt::TextDate) : QStringLiteral("?"),
                                  w.expires.isValid() ? w.expires.toLocalTime().toString(Qt::TextDate) : QStringLiteral("?")));
    }
    if (order.size() > kInboxCap)
        warningList_->addItem(QStringLiteral("… plus %1 more (zoom in or filter)").arg(order.size() - kInboxCap));
    if (order.isEmpty()) {
        auto* empty = new QListWidgetItem(QStringLiteral("No active warnings"), warningList_);
        empty->setFlags(empty->flags() & ~Qt::ItemIsSelectable);
        empty->setToolTip(QStringLiteral("The NWS feed answered and there is nothing to show."));
    }
    warningList_->setUpdatesEnabled(true);
}

void MainWindow::refreshTimelineStrip() {
    if (!timelineStrip_) return;
    const auto session = activeSession();
    if (!session || session->history.empty()) {
        timelineStrip_->setFrames({}, -1);
        timelineStrip_->setEvents({});
        return;
    }
    QVector<TimelineStrip::Frame> frames;
    frames.reserve(static_cast<qsizetype>(session->history.size()));
    for (const auto& frame : session->history) {
        frames.push_back({frame.dataTime, frame.failedChunks == 0,
                          session->bookmarks.contains(frame.volumeId), frame.volumeId});
    }
    timelineStrip_->setFrames(std::move(frames), session->frameIndex);
    QVector<QDateTime> events;
    events.reserve(latestWarnings_.size());
    for (const auto& warning : latestWarnings_) {
        if (warning.onset.isValid()) events.push_back(warning.onset);
    }
    timelineStrip_->setEvents(std::move(events));
}

void MainWindow::toggleFrameBookmark() {
    const auto session = activeSession();
    if (!session || session->frameIndex < 0 ||
        session->frameIndex >= static_cast<int>(session->history.size()))
        return;
    const auto& volumeId = session->history.at(static_cast<std::size_t>(session->frameIndex)).volumeId;
    if (session->bookmarks.contains(volumeId)) session->bookmarks.remove(volumeId);
    else session->bookmarks.insert(volumeId);
    QSettings().setValue(QStringLiteral("bookmarks/") + session->radarId,
                         session->bookmarks.values());
    refreshTimelineStrip();
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

void MainWindow::updateVelocityControls() {    if (!velocityModeCombo_ || !map_) return;
    const auto velocityProduct = map_->product() == ProductKind::Velocity;
    if (velocityBox_) velocityBox_->setVisible(velocityProduct);
    velocityModeCombo_->setEnabled(velocityProduct);
    const auto derived = velocityProduct && velocityModeCombo_->currentIndex() != 0;
    nyquistSpin_->setEnabled(derived);
    const auto stormRelative = velocityProduct && velocityModeCombo_->currentIndex() == 2;
    stormDirectionSpin_->setEnabled(stormRelative);
    stormSpeedSpin_->setEnabled(stormRelative);
}

void MainWindow::refreshCappiBox() {
    if (!cappiBox_ || !cappiHeightCombo_ || !map_) return;
    cappiBox_->setVisible(map_->product() == ProductKind::Cappi);
    const QSignalBlocker blocker(cappiHeightCombo_);
    const auto index = cappiHeightCombo_->findData(static_cast<int>(std::llround(cappiHeightM_)));
    cappiHeightCombo_->setCurrentIndex(index >= 0 ? index : 1);
}

void MainWindow::setCappiHeightM(const double heightM) {
    if (heightM <= 0.0 || heightM == cappiHeightM_) return;
    cappiHeightM_ = heightM;
    QSettings().setValue(QStringLiteral("view/cappiHeightM"), cappiHeightM_);
    refreshCappiBox();
    rederiveAllForCappiHeight();
}

void MainWindow::launchFrameRederive(const QString& radarId, const QString& volumeId,
                                     std::shared_ptr<DecodedChunk> base,
                                     const double radarElevationMeters,
                                     const double cappiHeightM) {
    const auto session = sessionFor(radarId, false);
    if (!session || !base || session->derivingVolumeIds.contains(volumeId)) return;
    session->derivingVolumeIds.insert(volumeId);
    const QPointer<MainWindow> guard(this);
    QThreadPool::globalInstance()->start(
        [guard, base, radarId, volumeId, radarElevationMeters, cappiHeightM] {
            auto derived = std::make_shared<DecodedChunk>(*base);
            appendDerivedProducts(*derived, radarElevationMeters, cappiHeightM);
            if (!guard) return;
            QMetaObject::invokeMethod(
                guard.data(),
                [guard, radarId, volumeId, derived, cappiHeightM] {
                    if (guard) guard->swapDerivedVolume(radarId, volumeId, derived, cappiHeightM);
                },
                Qt::QueuedConnection);
        });
}

void MainWindow::rederiveAllForCappiHeight() {
    // Height change: every resident frame carrying a stale CAPPI field gets
    // a background re-derive at the new altitude. Spilled frames are left
    // on disk and re-derive lazily when scrubbed to (showFrame checks the
    // per-frame tag). In-flight derives finish at their launch height;
    // their tag marks them stale so they re-derive once, correctly.
    for (const auto& radarId : radarSessions_.keys()) {
        const auto session = sessionFor(radarId, false);
        if (!session) continue;
        double radarElevationMeters = 0.0;
        const auto radarRow = model_ ? model_->rowForRadar(radarId) : -1;
        if (model_ && radarRow >= 0)
            radarElevationMeters = model_->siteAt(radarRow).antennaElevationMeters;
        for (const auto& frame : session->history) {
            if (!frame.chunk || frame.chunk->rays.empty()) continue;
            const bool hasCappi = std::any_of(
                frame.chunk->rays.cbegin(), frame.chunk->rays.cend(),
                [](const DecodedRay& ray) { return ray.has(ProductKind::Cappi); });
            if (!hasCappi) continue;
            if (!std::isnan(frame.cappiHeightM) && frame.cappiHeightM == cappiHeightM_) continue;
            launchFrameRederive(radarId, frame.volumeId, frame.chunk, radarElevationMeters,
                                cappiHeightM_);
        }
    }
    if (!selectedRadarId_.isEmpty()) {
        radarStatus_ = QStringLiteral("%1 re-deriving CAPPI at %2 km")
                           .arg(selectedRadarId_)
                           .arg(cappiHeightM_ / 1000.0, 0, 'f', 0);
        updateUnifiedStatus();
    }
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
    crossSectionStart_ = start;
    crossSectionEnd_ = end;
    crossSectionRadarId_ = selectedRadarId_;
    const auto& frame = session->history.at(static_cast<std::size_t>(session->frameIndex));
    const auto chunk = frameChunk(*session, static_cast<std::size_t>(session->frameIndex));
    if (!chunk) return;
    if (crossSectionDialog_ && crossSectionWidget_) {
        crossSectionDialog_->setWindowTitle(QStringLiteral("%1 Cross-section — live").arg(selectedRadarId_));
        crossSectionWidget_->setData(chunk, map_->product(), map_->palette(),
                                     map_->customPalette(), map_->velocityProcessing());
        crossSectionDialog_->show();
        crossSectionDialog_->raise();
        crossSectionDialog_->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("%1 Cross-section — live").arg(selectedRadarId_));
    dialog->resize(980, 620);
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    auto* note = new QLabel(
        QStringLiteral("Live: follows the timeline as new volumes arrive. Keep chasing — this window never blocks radar."),
        dialog);
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("groupCaption"));
    layout->addWidget(note);
    auto* crossSection = new CrossSectionWidget(
        chunk, {*site.latitude, *site.longitude}, site.antennaElevationMeters,
        start, end, map_->product(), map_->palette(), map_->customPalette(),
        map_->velocityProcessing(), dialog);
    layout->addWidget(crossSection, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* exportButton = buttons->addButton(QStringLiteral("Export CSV"), QDialogButtonBox::ActionRole);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        if (!crossSectionWidget_) return;
        const auto suggested = QStringLiteral("%1-cross-section-%2.csv")
                                   .arg(crossSectionRadarId_,
                                        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")));
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Export cross-section"),
                                                       suggested, QStringLiteral("CSV files (*.csv)"));
        if (!path.isEmpty() && !crossSectionWidget_->exportCsv(path))
            QMessageBox::warning(this, QStringLiteral("Export failed"), QStringLiteral("Could not write %1").arg(path));
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    crossSectionDialog_ = dialog;
    crossSectionWidget_ = crossSection;
    connect(dialog, &QObject::destroyed, this, [this] { crossSectionWidget_ = nullptr; });
    dialog->show();
}

void MainWindow::refreshCrossSection() {
    if (!crossSectionDialog_ || !crossSectionWidget_) return;
    if (crossSectionRadarId_ != selectedRadarId_) return;
    const auto session = activeSession();
    if (!session || session->frameIndex < 0 ||
        session->frameIndex >= static_cast<int>(session->history.size())) return;
    const auto& frame = session->history.at(static_cast<std::size_t>(session->frameIndex));
    // Only live-update when the transect product is still valid.
    if (isDerivedProduct(map_->product())) return;
    const auto chunk = frameChunk(*session, static_cast<std::size_t>(session->frameIndex));
    if (!chunk) return;
    crossSectionWidget_->setData(chunk, map_->product(), map_->palette(),
                                 map_->customPalette(), map_->velocityProcessing());
    crossSectionDialog_->setWindowTitle(
        QStringLiteral("%1 Cross-section — %2 (live)")
            .arg(crossSectionRadarId_,
                 frame.dataTime.toLocalTime().toString(QStringLiteral("HH:mm:ss"))));
}

void MainWindow::showScanDiagnostics() {
    if (scanDialog_) {
        populateScanDialog();
        scanDialog_->show();
        scanDialog_->raise();
        scanDialog_->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setModal(false);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(680, 420);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);
    scanSummary_ = new QLabel(dialog);
    scanSummary_->setWordWrap(true);
    scanSummary_->setObjectName(QStringLiteral("groupCaption"));
    layout->addWidget(scanSummary_);
    scanTable_ = new QTableWidget(dialog);
    scanTable_->setColumnCount(9);
    scanTable_->setHorizontalHeaderLabels({QStringLiteral("Tilt"), QStringLiteral("El"),
                                           QStringLiteral("Rays"), QStringLiteral("Max gap"),
                                           QStringLiteral("VAD wind"), QStringLiteral("Ht MSL"),
                                           QStringLiteral("Gates"), QStringLiteral("RMSE"),
                                           QStringLiteral("Fit")});
    scanTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    scanTable_->verticalHeader()->setVisible(false);
    scanTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    scanTable_->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(scanTable_, 1);
    auto* note = new QLabel(
        QStringLiteral("VAD is a layer-mean sine fit on dealiased velocity: guidance only, never "
                       "warnings. Poor and Invalid fits stay visible with their RMSE so a bad fit "
                       "never reads as calm air."),
        dialog);
    note->setWordWrap(true);
    note->setObjectName(QStringLiteral("groupCaption"));
    layout->addWidget(note);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* refreshButton = new QToolButton(dialog);
    refreshButton->setText(QStringLiteral("Refresh"));
    connect(refreshButton, &QToolButton::clicked, this, [this] { populateScanDialog(); });
    buttons->addWidget(refreshButton);
    auto* closeButton = new QToolButton(dialog);
    closeButton->setText(QStringLiteral("Close"));
    connect(closeButton, &QToolButton::clicked, dialog, &QDialog::close);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);
    scanDialog_ = dialog;
    connect(dialog, &QObject::destroyed, this, [this] {
        scanTable_ = nullptr;
        scanSummary_ = nullptr;
    });
    populateScanDialog();
    dialog->show();
}

void MainWindow::refreshScanDialog() {
    if (!scanDialog_) return;
    populateScanDialog();
}

void MainWindow::populateScanDialog() {
    if (!scanTable_ || !scanSummary_ || !map_) return;
    const auto chunk = map_->decodedChunk();
    const QString radar = selectedRadarId();
    scanDialog_->setWindowTitle(radar.isEmpty() ? QStringLiteral("Scan diagnostics")
                                                : QStringLiteral("%1 Scan diagnostics").arg(radar));
    if (!chunk || chunk->rays.empty() || radar.isEmpty()) {
        scanSummary_->setText(QStringLiteral("No volume loaded. Pick a station and go LIVE first."));
        scanTable_->setRowCount(0);
        return;
    }
    scanSummary_->setText(QStringLiteral("%1 · %2").arg(map_->scanDiagnosticsText(),
                                                        QStringLiteral("Volume follows the active panel")));
    const auto perTilt = map_->tiltDiagnostics();
    auto options = map_->velocityProcessing();
    options.mode = VelocityProcessingMode::Dealiased;
    const auto dealiased = processVelocity(*chunk, options);
    double radarElevationMeters = 0.0;
    if (model_) {
        const auto radarRow = model_->rowForRadar(radar);
        if (radarRow >= 0) radarElevationMeters = model_->siteAt(radarRow).antennaElevationMeters;
    }
    const auto levels = fitVadProfile(*chunk, dealiased, radarElevationMeters);
    scanTable_->setRowCount(static_cast<int>(perTilt.size()));
    for (int row = 0; row < static_cast<int>(perTilt.size()); ++row) {
        const auto& tilt = perTilt[static_cast<std::size_t>(row)];
        double elevationDegrees = std::numeric_limits<double>::quiet_NaN();
        for (const auto& ray : chunk->rays) {
            if (ray.elevationNumber == tilt.elevationNumber) {
                elevationDegrees = ray.elevationDegrees;
                break;
            }
        }
        const VadLevel* vad = nullptr;
        for (const auto& level : levels) {
            if (level.elevationNumber == tilt.elevationNumber) {
                vad = &level;
                break;
            }
        }
        const auto degree = QChar(0x00b0);
        auto cell = [this, row](int column, const QString& text, const QString& tooltip = {}) {
            auto* item = new QTableWidgetItem(text);
            if (!tooltip.isEmpty()) item->setToolTip(tooltip);
            scanTable_->setItem(row, column, item);
        };
        cell(0, QString::number(tilt.elevationNumber));
        cell(1, std::isnan(elevationDegrees) ? QStringLiteral("—")
                                             : QStringLiteral("%1%2").arg(elevationDegrees, 0, 'f', 1).arg(degree));
        cell(2, QString::number(tilt.rayCount));
        cell(3, QStringLiteral("%1%2").arg(tilt.largestAzimuthGapDegrees, 0, 'f', 1).arg(degree));
        QString windText = QStringLiteral("no VEL");
        QString windTip =
            QStringLiteral("This tilt carries no velocity moments, so there is nothing to fit.");
        QString heightText = QStringLiteral("—");
        QString gatesText = QStringLiteral("—");
        QString rmseText = QStringLiteral("—");
        QString fitText = QStringLiteral("n/a");
        QString fitTip;
        if (vad) {
            gatesText = QString::number(vad->gateCount);
            if (!std::isnan(vad->heightMslMeters))
                heightText = QStringLiteral("%1 km").arg(vad->heightMslMeters / 1000.0, 0, 'f', 1);
            if (!std::isnan(vad->rmseMetersPerSecond))
                rmseText = QStringLiteral("%1 m/s").arg(vad->rmseMetersPerSecond, 0, 'f', 1);
            switch (vad->quality) {
                case VadQuality::Good: fitText = QStringLiteral("Good"); break;
                case VadQuality::Marginal: fitText = QStringLiteral("Marginal"); break;
                case VadQuality::Poor: fitText = QStringLiteral("Poor"); break;
                case VadQuality::Invalid: fitText = QStringLiteral("Invalid"); break;
            }
            fitTip = QStringLiteral("Good: 200+ gates, 270°+ circle, RMSE ≤ 3 m/s. Marginal: 60+ "
                                    "gates, 180°+, RMSE ≤ 6. Poor/Invalid: shown, not trusted.");
            if (vad->quality == VadQuality::Invalid) {
                windText = QStringLiteral("no fit");
                windTip = QStringLiteral("Too few gates, too narrow a circle, or degenerate "
                                         "geometry: no wind was fitted.");
            } else {
                windText = QStringLiteral("%1%2 / %3 m/s")
                               .arg(vad->directionFromDegrees, 0, 'f', 0)
                               .arg(degree)
                               .arg(vad->speedMetersPerSecond, 0, 'f', 1);
                windTip = QStringLiteral("Layer-mean wind (FROM direction) from the sine fit. "
                                         "Guidance only.");
            }
        }
        cell(4, windText, windTip);
        cell(5, heightText);
        cell(6, gatesText);
        cell(7, rmseText);
        cell(8, fitText, fitTip);
    }
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
    refreshPalettePreview();
    statusBar()->showMessage(QStringLiteral("Imported %1 palette stops from %2").arg(stops.size()).arg(path), 5000);
}

void MainWindow::refreshPalettePreview() {
    // PalettePreview lives in the anonymous namespace above; the member is
    // stored as QWidget so the header stays free of the paint details.
    if (auto* preview = static_cast<PalettePreview*>(palettePreview_)) {
        preview->setPalette(static_cast<RadarMapCanvas::PaletteKind>(paletteCombo_->currentIndex()),
                            map_->customPalette());
    }
}

void MainWindow::exportSnapshot() {
    const auto defaultName = QStringLiteral("%1-%2-%3.png")
                                 .arg(selectedRadarId_, QString::fromUtf8(productDescriptor(map_->product()).shortName.data()),
                                      QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")));
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Export map snapshot"), defaultName,
                                                   QStringLiteral("PNG image (*.png)"));
    if (path.isEmpty()) return;
    const auto image = map_->grab();
    if (!image.save(path, "PNG")) {
        QMessageBox::warning(this, QStringLiteral("Export failed"), QStringLiteral("Could not write %1").arg(path));
        return;
    }
    // GIS sidecar: the grab corners match the panel bounds exactly, so the
    // six-line world file georeferences the PNG (baked-in legend included).
    const auto bounds = map_->visibleLonLatBounds();
    QString worldError;
    const auto world = bounds.valid
                           ? frameWorldFile(bounds.north, bounds.south, bounds.east, bounds.west,
                                            image.width(), image.height(), &worldError)
                           : QString{};
    if (world.isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("Snapshot exported (no world file: %1)")
                .arg(bounds.valid ? worldError : QStringLiteral("map view not ready")),
            5000);
        return;
    }
    QFile worldFile(QFileInfo(path).dir().filePath(QFileInfo(path).completeBaseName() +
                                                   QStringLiteral(".pgw")));
    if (!worldFile.open(QIODevice::WriteOnly | QIODevice::Text) ||
        worldFile.write(world.toUtf8()) != world.toUtf8().size()) {
        statusBar()->showMessage(
            QStringLiteral("Snapshot exported (world file failed: %1)").arg(worldFile.errorString()),
            5000);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Snapshot exported with world file"), 5000);
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
    const auto total = static_cast<int>(session->history.size());
    QProgressDialog progress(QStringLiteral("Rendering %1 frames…").arg(total), QStringLiteral("Cancel"),
                             0, total, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(250);
    progress.setValue(0);
    QVector<QImage> frames;
    frames.reserve(static_cast<qsizetype>(total));
    for (int index = 0; index < total; ++index) {
        if (progress.wasCanceled()) {
            showFrame(previousIndex);
            statusBar()->showMessage(QStringLiteral("Timeline export cancelled"), 4000);
            return;
        }
        showFrame(index);
        progress.setValue(index + 1);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        frames.push_back(map_->grab().toImage());
    }
    progress.setValue(total);
    showFrame(previousIndex);
    QString error;
    const auto delay = std::max(5, speedCombo_->currentData().toInt() / 10);
    if (!writeAnimatedGif(path, frames, delay, &error)) {
        QMessageBox::warning(this, QStringLiteral("Export failed"), error);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Animated timeline exported to %1").arg(path), 5000);
}

void MainWindow::exportTimelineMp4() {
#if defined(CAPPEDWEATHER_HAS_MULTIMEDIA)
    const auto session = activeSession();
    if (!session || session->history.empty()) return;
    auto* windowHandle = window()->windowHandle();
    if (!windowHandle) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
                             QStringLiteral("The window is not ready for capture yet."));
        return;
    }
    const auto defaultName = QStringLiteral("%1-%2-timeline.mp4")
                                 .arg(selectedRadarId_, QString::fromUtf8(productDescriptor(map_->product()).shortName.data()));
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Record timeline video"),
                                                   defaultName, QStringLiteral("MP4 video (*.mp4)"));
    if (path.isEmpty()) return;
    // Realtime window capture through the platform backend (MediaFoundation
    // on Windows): each frame holds on screen for the playback interval
    // while the recorder runs. Frame-exact output stays the GIF path's job —
    // this is the sharing-friendly recording. WebM/VP9 is not guaranteed by
    // any platform backend, so MP4/H.264 is the portable target.
    QWindowCapture capture;
    capture.setWindow(windowHandle);
    QMediaCaptureSession captureSession;
    captureSession.setWindowCapture(&capture);
    QMediaRecorder recorder;
    recorder.setMediaFormat(QMediaFormat(QMediaFormat::MPEG4));
    recorder.setQuality(QMediaRecorder::HighQuality);
    recorder.setOutputLocation(QUrl::fromLocalFile(path));
    captureSession.setRecorder(&recorder);
    const auto previousIndex = session->frameIndex;
    const auto total = static_cast<int>(session->history.size());
    const auto holdMs = std::max(400, speedCombo_->currentData().toInt());
    QProgressDialog progress(QStringLiteral("Recording %1 frames…").arg(total), QStringLiteral("Cancel"),
                             0, total, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(250);
    progress.setValue(0);
    capture.start();
    recorder.record();
    for (int index = 0; index < total; ++index) {
        if (progress.wasCanceled() ||
            recorder.recorderState() == QMediaRecorder::StoppedState) {
            break;
        }
        showFrame(index);
        progress.setValue(index + 1);
        QEventLoop wait;
        QTimer::singleShot(holdMs, &wait, &QEventLoop::quit);
        wait.exec(QEventLoop::ExcludeUserInputEvents);
    }
    recorder.stop();
    capture.stop();
    progress.setValue(total);
    showFrame(previousIndex);
    if (recorder.error() != QMediaRecorder::NoError) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
                             QStringLiteral("Recorder error: %1").arg(recorder.errorString()));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Timeline video recorded to %1").arg(path), 5000);
#else
    QMessageBox::information(
        this, QStringLiteral("Timeline MP4"),
        QStringLiteral("Video export needs a CappedWeather build with Qt Multimedia. "
                       "Timeline GIF works in every build."));
#endif
}

void MainWindow::exportTimelineWebM() {
    const auto session = activeSession();
    if (!session || session->history.empty()) return;
    const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("Timeline WebM"),
            QStringLiteral("WebM export pipes frames through ffmpeg, which was not found on PATH. "
                           "Install ffmpeg (https://ffmpeg.org/download.html) and retry. "
                           "Timeline GIF works without it."));
        return;
    }
    const auto defaultName = QStringLiteral("%1-%2-timeline.webm")
                                 .arg(selectedRadarId_, QString::fromUtf8(productDescriptor(map_->product()).shortName.data()));
    const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Export timeline WebM"),
                                                   defaultName, QStringLiteral("WebM video (*.webm)"));
    if (path.isEmpty()) return;
    // Same frame timing as the GIF path: centisecond delay from the playback
    // speed, converted to a pipe framerate.
    const auto delayCs = std::max(5, speedCombo_->currentData().toInt() / 10);
    const auto frameRate = 100.0 / static_cast<double>(delayCs);
    QProcess encoder;
    encoder.setProgram(ffmpeg);
    encoder.setArguments({QStringLiteral("-y"), QStringLiteral("-f"), QStringLiteral("image2pipe"),
                          QStringLiteral("-framerate"), QString::number(frameRate),
                          QStringLiteral("-i"), QStringLiteral("-"),
                          QStringLiteral("-c:v"), QStringLiteral("libvpx-vp9"),
                          QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                          QStringLiteral("-crf"), QStringLiteral("28"),
                          QStringLiteral("-b:v"), QStringLiteral("0"), path});
    encoder.start();
    if (!encoder.waitForStarted(10000)) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
                             QStringLiteral("Could not start ffmpeg: %1").arg(encoder.errorString()));
        return;
    }
    const auto previousIndex = session->frameIndex;
    const auto total = static_cast<int>(session->history.size());
    QProgressDialog progress(QStringLiteral("Encoding %1 frames…").arg(total), QStringLiteral("Cancel"),
                             0, total, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(250);
    progress.setValue(0);
    bool failed = false;
    QString failure;
    for (int index = 0; index < total; ++index) {
        if (progress.wasCanceled()) {
            encoder.kill();
            failed = true;
            failure = QStringLiteral("Timeline export cancelled");
            break;
        }
        showFrame(index);
        progress.setValue(index + 1);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!map_->grab().toImage().save(&buffer, "PNG")) {
            encoder.kill();
            failed = true;
            failure = QStringLiteral("Could not grab frame %1").arg(index + 1);
            break;
        }
        // The pipe can accept fewer bytes than offered: loop until the whole
        // PNG is queued, or fail closed instead of a truncated frame stream.
        qint64 queued = 0;
        while (queued < png.size()) {
            const qint64 accepted = encoder.write(png.constData() + queued, png.size() - queued);
            if (accepted < 0) break;
            queued += accepted;
            if (queued < png.size() && !encoder.waitForBytesWritten(10000)) break;
        }
        if (queued != png.size()) {
            encoder.kill();
            failed = true;
            failure = QStringLiteral("ffmpeg pipe broke at frame %1: %2").arg(index + 1).arg(
                encoder.errorString());
            break;
        }
    }
    encoder.closeWriteChannel();
    if (!failed && !encoder.waitForFinished(60000)) {
        encoder.kill();
        failed = true;
        failure = QStringLiteral("ffmpeg did not finish: %1").arg(encoder.errorString());
    }
    progress.setValue(total);
    showFrame(previousIndex);
    if (failed || encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0) {
        QFile::remove(path);
        if (failed && failure == QStringLiteral("Timeline export cancelled")) {
            statusBar()->showMessage(failure, 4000);
        } else {
            const auto details = QString::fromUtf8(encoder.readAllStandardError()).trimmed();
            QMessageBox::warning(this, QStringLiteral("Export failed"),
                                 failed ? failure
                                        : QStringLiteral("ffmpeg failed (exit %1)%2")
                                              .arg(encoder.exitCode())
                                              .arg(details.isEmpty() ? QString{}
                                                                     : QStringLiteral(": %1").arg(
                                                                           details.left(300))));
        }
        return;
    }
    statusBar()->showMessage(QStringLiteral("Timeline WebM exported to %1").arg(path), 5000);
}

QString MainWindow::frameGroundOverlayKml(const QString& name, const QString& pngHref, double north,
                                         double south, double east, double west, QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QString{};
    };
    if (name.isEmpty() || pngHref.isEmpty()) return fail(QStringLiteral("Overlay needs a name and image"));
    for (const double edge : {north, south, east, west}) {
        if (!std::isfinite(edge)) return fail(QStringLiteral("Map bounds are not finite yet"));
    }
    if (!(north > south)) return fail(QStringLiteral("Map bounds are flipped north/south"));
    if (!(east > west)) {
        // Antimeridian wrap: a single LatLonBox cannot express it, and a
        // flipped box would silently mirror the image. No output instead.
        return fail(QStringLiteral("Map view crosses the antimeridian"));
    }
    const auto xml = [](QString raw) { return raw.replace(QLatin1Char('&'), QStringLiteral("&amp;"))
                                                     .replace(QLatin1Char('<'), QStringLiteral("&lt;"))
                                                     .replace(QLatin1Char('>'), QStringLiteral("&gt;")); };
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n"
                          "<GroundOverlay>\n"
                          "<name>%1</name>\n"
                          "<Icon><href>%2</href></Icon>\n"
                          "<LatLonBox><north>%3</north><south>%4</south>"
                          "<east>%5</east><west>%6</west><rotation>0</rotation></LatLonBox>\n"
                          "</GroundOverlay>\n"
                          "</kml>\n")
        .arg(xml(name), xml(pngHref))
        .arg(north, 0, 'f', 6)
        .arg(south, 0, 'f', 6)
        .arg(east, 0, 'f', 6)
        .arg(west, 0, 'f', 6);
}

QString MainWindow::frameWorldFile(const double north, const double south, const double east,
                                  const double west, const int imageWidth, const int imageHeight,
                                  QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return QString{};
    };
    for (const double edge : {north, south, east, west}) {
        if (!std::isfinite(edge)) return fail(QStringLiteral("Map bounds are not finite yet"));
    }
    if (imageWidth <= 0 || imageHeight <= 0)
        return fail(QStringLiteral("Snapshot has no pixels yet"));
    if (!(north > south)) return fail(QStringLiteral("Map bounds are flipped north/south"));
    if (!(east > west)) return fail(QStringLiteral("Map view crosses the antimeridian"));
    // Pixel-center convention: the top-left pixel's center sits half a pixel
    // inside the corner, and latitude steps negative going down rows.
    const double pixelWidth = (east - west) / static_cast<double>(imageWidth);
    const double pixelHeight = -(north - south) / static_cast<double>(imageHeight);
    return QStringLiteral("%1\n0\n0\n%2\n%3\n%4\n")
        .arg(pixelWidth, 0, 'f', 10)
        .arg(pixelHeight, 0, 'f', 10)
        .arg(west + pixelWidth / 2.0, 0, 'f', 10)
        .arg(north + pixelHeight / 2.0, 0, 'f', 10);
}

void MainWindow::exportFrameKml() {
    if (!map_) return;
    const auto bounds = map_->visibleLonLatBounds();
    QString boundsError;
    if (!bounds.valid) boundsError = QStringLiteral("Map view is not ready for export yet");
    const auto defaultName = QStringLiteral("%1-%2-frame")
                                 .arg(selectedRadarId_, QString::fromUtf8(productDescriptor(map_->product()).shortName.data()));
    const auto kmlPath = QFileDialog::getSaveFileName(this, QStringLiteral("Export frame for GIS"),
                                                      defaultName, QStringLiteral("KML (*.kml)"));
    if (kmlPath.isEmpty()) return;
    if (!bounds.valid) {
        QMessageBox::warning(this, QStringLiteral("Export failed"), boundsError);
        return;
    }
    const auto pngPath = QFileInfo(kmlPath).dir().filePath(
        QFileInfo(kmlPath).completeBaseName() + QStringLiteral(".png"));
    const auto frame = map_->grab();
    if (!frame.save(pngPath, "PNG")) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
                             QStringLiteral("Could not write %1").arg(pngPath));
        return;
    }
    // Same world-file sidecar as snapshots, so the KML image doubles as a
    // standalone GIS raster. A failed sidecar warns but keeps the KML.
    QString worldError;
    const auto world = frameWorldFile(bounds.north, bounds.south, bounds.east, bounds.west,
                                      frame.width(), frame.height(), &worldError);
    QString worldNote;
    if (world.isEmpty()) {
        worldNote = QStringLiteral(" (no world file: %1)").arg(worldError);
    } else {
        QFile worldFile(QFileInfo(kmlPath).dir().filePath(
            QFileInfo(kmlPath).completeBaseName() + QStringLiteral(".pgw")));
        if (!worldFile.open(QIODevice::WriteOnly | QIODevice::Text) ||
            worldFile.write(world.toUtf8()) != world.toUtf8().size()) {
            worldNote = QStringLiteral(" (world file failed: %1)").arg(worldFile.errorString());
        } else {
            worldNote = QStringLiteral(" + world file");
        }
    }
    const auto overlayName = QStringLiteral("%1 %2 %3")
                                 .arg(selectedRadarId_,
                                      QString::fromUtf8(productDescriptor(map_->product()).shortName.data()),
                                      QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    QString kmlError;
    const auto kml = frameGroundOverlayKml(overlayName, QFileInfo(pngPath).fileName(), bounds.north,
                                           bounds.south, bounds.east, bounds.west, &kmlError);
    if (kml.isEmpty()) {
        QFile::remove(pngPath);
        QMessageBox::warning(this, QStringLiteral("Export failed"), kmlError);
        return;
    }
    QFile file(kmlPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(kml.toUtf8()) != kml.toUtf8().size()) {
        QFile::remove(pngPath);
        QMessageBox::warning(this, QStringLiteral("Export failed"),
                             QStringLiteral("Could not write %1").arg(kmlPath));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Frame exported for GIS: %1 + image%2").arg(kmlPath, worldNote), 5000);
}

void MainWindow::exportCasePack() {
    const auto session = activeSession();
    const QString radar = selectedRadarId();
    if (!session || session->history.empty() || radar.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Export case-pack"),
                                 QStringLiteral("Load a radar volume first, then pack the case."));
        return;
    }
    if (!zipAvailable()) {
        QMessageBox::information(this, QStringLiteral("Export case-pack"),
                                 QStringLiteral("Case-packs need a CappedWeather build with zlib. "
                                                "This build decoded without it."));
        return;
    }
    const auto packPath = QFileDialog::getSaveFileName(this, QStringLiteral("Export case-pack"),
                                                       QStringLiteral("%1-case.cwcase").arg(radar),
                                                       QStringLiteral("Case-pack (*.cwcase)"));
    if (packPath.isEmpty()) return;
    QTemporaryDir staging;
    if (!staging.isValid()) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
                             QStringLiteral("Could not stage the case-pack"));
        return;
    }
    const auto total = static_cast<int>(session->history.size());
    QProgressDialog progress(QStringLiteral("Packing %1 volumes…").arg(total), QStringLiteral("Cancel"),
                             0, total, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(250);
    progress.setValue(0);
    CaseManifest manifest;
    manifest.radarId = radar;
    manifest.frameIndex = session->frameIndex;
    manifest.exportedAt = QDateTime::currentDateTimeUtc();
    QVector<QPair<QString, QString>> entries;
    const auto sanitize = [](const QString& raw) {
        QString safe;
        for (const auto ch : raw)
            safe.push_back(ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_') ||
                                   ch == QLatin1Char('.')
                               ? ch
                               : QLatin1Char('_'));
        if (safe.isEmpty()) safe = QStringLiteral("volume");
        return safe;
    };
    for (int i = 0; i < total; ++i) {
        if (progress.wasCanceled()) {
            statusBar()->showMessage(QStringLiteral("Case-pack export cancelled"), 4000);
            return;
        }
        const auto& frame = session->history.at(static_cast<std::size_t>(i));
        const QString zipName =
            QStringLiteral("volumes/%1.cwdv").arg(sanitize(frame.volumeId));
        const QString staged = staging.filePath(zipName);
        QDir().mkpath(QFileInfo(staged).absolutePath());
        bool staged_ok = false;
        if (frame.chunk) {
            staged_ok = spillVolume(*frame.chunk, staged.toStdString());
        } else if (!frame.spilledPath.isEmpty()) {
            staged_ok = QFile::copy(frame.spilledPath, staged);
        }
        if (!staged_ok) {
            QMessageBox::warning(
                this, QStringLiteral("Export failed"),
                QStringLiteral("Could not stage volume %1: it is neither resident nor on disk.")
                    .arg(frame.volumeId));
            return;
        }
        CaseFrame packed;
        packed.volumeId = frame.volumeId;
        packed.dataTime = frame.dataTime;
        packed.chunkCount = frame.chunkCount;
        packed.failedChunks = frame.failedChunks;
        packed.file = zipName;
        manifest.frames.push_back(packed);
        entries.push_back({zipName, staged});
        progress.setValue(i + 1);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    for (auto* panel : maps_) {
        CasePanelState state;
        state.radarId = panel->radarId();
        state.product = static_cast<int>(panel->product());
        const auto tilt = panel->elevationFilter();
        state.tilt = tilt ? *tilt : -1;
        manifest.panels.push_back(state);
    }
    int placefileNumber = 0;
    for (const auto& source : placefileSource_->sourceUrls()) {
        const QUrl url(source);
        if (url.isLocalFile()) {
            const QString local = url.toLocalFile();
            if (QFileInfo(local).size() > 2 * 1024 * 1024) continue;  // same 2 MiB guard
            const QString zipName =
                QStringLiteral("placefiles/%1.txt").arg(placefileNumber++);
            const QString staged = staging.filePath(zipName);
            QDir().mkpath(QFileInfo(staged).absolutePath());
            if (!QFile::copy(local, staged)) continue;
            manifest.placefileFiles.push_back(zipName);
            entries.push_back({zipName, staged});
        } else if (url.isValid() && (url.scheme() == QStringLiteral("http") ||
                                     url.scheme() == QStringLiteral("https"))) {
            manifest.placefileUrls.push_back(source);
        }
    }
    QFile manifestFile(staging.filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        manifestFile.write(writeCaseManifest(manifest)) <= 0) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
                             QStringLiteral("Could not write the case manifest"));
        return;
    }
    manifestFile.close();
    entries.push_front({QStringLiteral("manifest.json"), manifestFile.fileName()});
    progress.setValue(total);
    QString zipError;
    if (!zipPackFiles(entries, packPath, &zipError)) {
        QMessageBox::warning(this, QStringLiteral("Export failed"), zipError);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Case-pack exported: %1 volumes for %2").arg(total).arg(radar), 5000);
}

void MainWindow::importCasePack() {
    if (!zipAvailable()) {
        QMessageBox::information(this, QStringLiteral("Import case-pack"),
                                 QStringLiteral("Case-packs need a CappedWeather build with zlib. "
                                                "This build decoded without it."));
        return;
    }
    const auto packPath = QFileDialog::getOpenFileName(this, QStringLiteral("Import case-pack"), {},
                                                       QStringLiteral("Case-pack (*.cwcase)"));
    if (packPath.isEmpty()) return;
    QTemporaryDir unpacked;
    if (!unpacked.isValid()) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QStringLiteral("Could not stage the case-pack"));
        return;
    }
    QString zipError;
    const auto names = zipExtractFiles(packPath, unpacked.path(), &zipError);
    if (names.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             zipError.isEmpty() ? QStringLiteral("Empty case-pack") : zipError);
        return;
    }
    if (!names.contains(QStringLiteral("manifest.json"))) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
                             QStringLiteral("Case-pack has no manifest"));
        return;
    }
    QFile manifestFile(unpacked.filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Import failed"), manifestFile.errorString());
        return;
    }
    QString manifestError;
    const auto manifest = parseCaseManifest(manifestFile.readAll(), &manifestError);
    if (!manifest) {
        QMessageBox::warning(this, QStringLiteral("Import failed"), manifestError);
        return;
    }
    for (const auto& frame : manifest->frames) {
        if (!names.contains(frame.file)) {
            QMessageBox::warning(this, QStringLiteral("Import failed"),
                                 QStringLiteral("Case-pack is missing %1").arg(frame.file));
            return;
        }
    }
    const QString radar = manifest->radarId;
    auto session = sessionFor(radar);
    if (!session) return;
    if (!session->history.empty()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Import case-pack"),
            QStringLiteral("Replace the %1 history (%2 volumes) with the packed case (%3 volumes)?")
                .arg(radar)
                .arg(session->history.size())
                .arg(manifest->frames.size()));
        if (answer != QMessageBox::Yes) return;
        for (const auto& frame : session->history) {
            if (!frame.spilledPath.isEmpty()) QFile::remove(frame.spilledPath);
        }
    }
    // Session rebuild is shared with the unit test (populateImportedSession):
    // volumes stage through the spill path and validate before the session
    // is touched, so a bad pack leaves the live history alone.
    session->radarId = radar;
    QString populateError;
    if (!populateImportedSession(*session, *manifest, unpacked.path(), &populateError)) {
        QMessageBox::warning(this, QStringLiteral("Import failed"), populateError);
        return;
    }
    for (int i = 0; i < static_cast<int>(manifest->panels.size()) && i < maps_.size(); ++i) {
        const auto& state = manifest->panels.at(i);
        auto* panel = maps_.at(i);
        if (!state.radarId.isEmpty()) panel->setSelectedRadar(state.radarId);
        if (state.product >= 0 && state.product < static_cast<int>(productDescriptors().size()))
            panel->setProduct(static_cast<ProductKind>(state.product));
        panel->setElevationFilter(state.tilt >= 0 ? std::optional<int>(state.tilt) : std::nullopt);
    }
    for (const auto& file : manifest->placefileFiles) {
        QString placefileError;
        placefileSource_->addLocalFile(unpacked.filePath(file), &placefileError);
    }
    for (const auto& source : manifest->placefileUrls) {
        const QUrl url(source);
        if (url.isValid()) {
            QString urlError;
            placefileSource_->addUrl(url, &urlError);
        }
    }
    if (radar == selectedRadarId_) {
        updateProductAvailability();
        rebuildTiltCombo();
        showFrame(session->frameIndex);
    }
    statusBar()->showMessage(
        QStringLiteral("Case-pack imported: %1 volumes for %2").arg(session->history.size()).arg(radar),
        5000);
}

void MainWindow::downloadOfflinePack() {
    double latitude = 35.0;
    double longitude = -97.0;
    QString centerName = QStringLiteral("Southern Plains");
    if (model_) {
        const auto row = model_->rowForRadar(selectedRadarId_);
        if (row >= 0) {
            const auto& site = model_->siteAt(row);
            if (site.latitude && site.longitude) {
                latitude = *site.latitude;
                longitude = *site.longitude;
                centerName = selectedRadarId_;
            }
        }
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Download offline map pack"));
    dialog.setModal(true);
    auto* layout = new QVBoxLayout(&dialog);
    auto* centerLabel = new QLabel(
        QStringLiteral("Center: %1 (%2, %3). Tiles land in the shared map cache, "
                       "so the chase works with no signal.")
            .arg(centerName)
            .arg(latitude, 0, 'f', 2)
            .arg(longitude, 0, 'f', 2),
        &dialog);
    centerLabel->setWordWrap(true);
    layout->addWidget(centerLabel);
    auto* form = new QFormLayout();
    auto* halfSpin = new QDoubleSpinBox(&dialog);
    halfSpin->setRange(0.5, 8.0);
    halfSpin->setSingleStep(0.5);
    halfSpin->setValue(2.0);
    halfSpin->setSuffix(QStringLiteral("°"));
    form->addRow(QStringLiteral("Half-size"), halfSpin);
    auto* minZoomSpin = new QSpinBox(&dialog);
    minZoomSpin->setRange(0, 12);
    minZoomSpin->setValue(4);
    form->addRow(QStringLiteral("Min zoom"), minZoomSpin);
    auto* maxZoomSpin = new QSpinBox(&dialog);
    maxZoomSpin->setRange(0, 14);
    maxZoomSpin->setValue(8);
    form->addRow(QStringLiteral("Max zoom"), maxZoomSpin);
    layout->addLayout(form);
    auto* estimateLabel = new QLabel(&dialog);
    estimateLabel->setWordWrap(true);
    estimateLabel->setObjectName(QStringLiteral("groupCaption"));
    layout->addWidget(estimateLabel);
    const auto refreshEstimate = [&] {
        const auto plan = planOfflinePack(latitude, longitude, halfSpin->value(),
                                          minZoomSpin->value(), maxZoomSpin->value());
        if (!plan.error.isEmpty()) {
            estimateLabel->setText(plan.error);
            return plan;
        }
        estimateLabel->setText(
            QStringLiteral("%1 tiles, ~%2 MB. Roads stay readable offline to zoom %3.")
                .arg(plan.tiles.size())
                .arg(plan.estimatedBytes / 1048576.0, 0, 'f', 1)
                .arg(plan.maxZoom));
        return plan;
    };
    connect(halfSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog,
            [&](double) { refreshEstimate(); });
    connect(minZoomSpin, qOverload<int>(&QSpinBox::valueChanged), &dialog,
            [&](int) { refreshEstimate(); });
    connect(maxZoomSpin, qOverload<int>(&QSpinBox::valueChanged), &dialog,
            [&](int) { refreshEstimate(); });
    refreshEstimate();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Download"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto plan =
        planOfflinePack(latitude, longitude, halfSpin->value(), minZoomSpin->value(),
                        maxZoomSpin->value());
    if (!plan.error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Offline map pack"), plan.error);
        return;
    }
    QProgressDialog progress(QStringLiteral("Caching %1 tiles…").arg(plan.tiles.size()),
                             QStringLiteral("Cancel"), 0,
                             static_cast<int>(plan.tiles.size()) * plan.urlsPerTile, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(250);
    progress.setValue(0);
    // Local event loop: the packer is async, but this flow is modal like
    // the GIF/WebM exports. The packer outlives the loop via deleteLater.
    QEventLoop wait;
    QString packMessage;
    bool packOk = false;
    auto* packer = new OfflinePacker(this);
    connect(packer, &OfflinePacker::progressed, &progress,
            [&](int done, int) { progress.setValue(done); });
    connect(packer, &OfflinePacker::finished, &wait,
            [&](bool ok, const QString& message) {
                packOk = ok;
                packMessage = message;
                progress.setValue(progress.maximum());
                packer->deleteLater();
                wait.quit();
            });
    connect(&progress, &QProgressDialog::canceled, packer, &OfflinePacker::cancel);
    packer->start(plan);
    wait.exec(QEventLoop::ExcludeUserInputEvents);
    if (packOk) {
        statusBar()->showMessage(QStringLiteral("Offline maps: %1").arg(packMessage), 8000);
    } else if (packMessage != QStringLiteral("Offline pack cancelled")) {
        QMessageBox::warning(this, QStringLiteral("Offline map pack"), packMessage);
    } else {
        statusBar()->showMessage(packMessage, 4000);
    }
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
    maximumHistoryFrames_ = std::max(maximumHistoryFrames_, std::clamp(requestedFrames, 2, 32));
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
    if (paths.size() > 32) paths = paths.sliced(paths.size() - 32);
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
    if (paths.size() > 32) paths = paths.sliced(paths.size() - 32);
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
    maximum->setRange(1, 32);
    maximum->setValue(24);
    maximum->setToolTip(QStringLiteral(
        "Matching volumes are evenly sampled into the in-memory case timeline (up to 32 frames)"));
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
        case RadarHealthState::Unknown: default: color = QStringLiteral("#7d838c"); text = QStringLiteral("Unknown"); break;
    }
    // Border tracks the active theme; a hardcoded Classic grey ring was left
    // behind on the Squall theme after a live switch.
    const auto ring = theme_ == AppTheme::Prototype ? QStringLiteral("#39424f")
                                                    : QStringLiteral("#3f444d");
    healthDot_->setStyleSheet(QStringLiteral(
        "background-color: %1; border: 1px solid %2; border-radius: 6px;").arg(color, ring));
    healthLabel_->setText(text);
    if (!tooltip.isEmpty()) {
        healthDot_->setToolTip(tooltip);
        healthLabel_->setToolTip(tooltip);
    }
}

RadarHealthState MainWindow::classifyHealth() const {
    if (selectedRadarId_.isEmpty()) return RadarHealthState::Unknown;
    if (radarStatus_.contains(QStringLiteral("error"), Qt::CaseInsensitive)) return RadarHealthState::Offline;
    const auto now = QDateTime::currentDateTimeUtc();
    // Arrival recency answers "is the pipeline delivering". Grading on data
    // age alone punished healthy radars: a fresh NEXRAD volume is routinely
    // 5-10 min old at completion (scan + S3 upload + poll + download), so
    // live stations flipped Stale/Offline on every slow volume. NEXRAD
    // cadence is 4-10 min, so 15 min without any arrival means the feed, not
    // the clock, is the problem. But arrival alone is not enough either: a
    // dead feed's first backfill arrives "now" carrying days-old data, so
    // Online also requires the data itself to be fresh (30 min bound leaves
    // 2x margin over the slowest healthy pipeline).
    const auto arrivalAge =
        lastArrivalTime_.isValid() ? lastArrivalTime_.secsTo(now) : std::numeric_limits<qint64>::max();
    const auto dataAge =
        lastVolumeTime_.isValid() ? lastVolumeTime_.secsTo(now) : std::numeric_limits<qint64>::max();
    if (arrivalAge <= 900 && dataAge <= 1800) return RadarHealthState::Online;
    if (dataAge == std::numeric_limits<qint64>::max()) return RadarHealthState::Stale;
    if (dataAge <= 300) return RadarHealthState::Online;     // <= 5 min
    if (dataAge <= 900) return RadarHealthState::Stale;      // 5-15 min
    return RadarHealthState::Offline;
}

QString healthAgeText(const qint64 seconds) {
    const auto age = std::max<qint64>(0, seconds);
    if (age < 60) return QStringLiteral("%1s").arg(age);
    if (age < 3600) return QStringLiteral("%1m").arg(age / 60);
    return QStringLiteral("%1h %2m").arg(age / 3600).arg((age % 3600) / 60);
}

void MainWindow::recomputeRadarHealth() {
    // The dot answers "is data flowing"; the tooltip says how old the data
    // behind it is, so a station serving old-but-arriving volumes reads
    // Online with an honest age instead of a confusing Offline.
    QString tip;
    if (lastVolumeTime_.isValid()) {
        const auto now = QDateTime::currentDateTimeUtc();
        tip = QStringLiteral("Latest scan %1 ago").arg(healthAgeText(lastVolumeTime_.secsTo(now)));
        if (lastArrivalTime_.isValid())
            tip += QStringLiteral(" · received %1 ago")
                       .arg(healthAgeText(lastArrivalTime_.secsTo(now)));
    }
    setRadarHealth(classifyHealth(), tip);
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
