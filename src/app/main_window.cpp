#include "app/main_window.hpp"

#include "app/nexrad_source.hpp"
#include "app/radar_map_canvas.hpp"
#include "app/radar_list_model.hpp"
#include "app/weather_overlay_source.hpp"
#include "core/level2_decoder.hpp"
#include "core/product.hpp"

#include <QComboBox>
#include <QCheckBox>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QScreen>
#include <QSlider>
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

    connect(map_, &RadarMapCanvas::radarClicked, this, [this](const QString& radarId) {
        const auto row = model_->rowForRadar(radarId);
        if (row >= 0) {
            radarSelected(model_->index(row, 0));
        }
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
    connect(frameSlider_, &QSlider::valueChanged, this, &MainWindow::showFrame);
    connect(previousButton_, &QToolButton::clicked, this, [this] {
        stopAnimation();
        showFrame(std::max(0, frameIndex_ - 1));
    });
    connect(nextButton_, &QToolButton::clicked, this, [this] {
        stopAnimation();
        showFrame(std::min(static_cast<int>(history_.size()) - 1, frameIndex_ + 1));
    });
    connect(playButton_, &QToolButton::toggled, this, [this](const bool playing) {
        if (!playing || history_.size() < 2) {
            stopAnimation();
            return;
        }
        if (frameIndex_ >= static_cast<int>(history_.size()) - 1) showFrame(0);
        playButton_->setText(QStringLiteral("Pause"));
        animationTimer_->start(speedCombo_->currentData().toInt());
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
    connect(source_, &NexradSource::chunkDecompressed, this, &MainWindow::chunkDecompressed);
    connect(source_, &NexradSource::volumeReady, this, &MainWindow::volumeReady);
    connect(overlaySource_, &WeatherOverlaySource::overlaysReady, this,
            [this](QVector<WeatherWarning> warnings, QVector<StormReport> reports, const QDateTime&) {
                map_->setWeatherOverlays(std::move(warnings), std::move(reports));
            });
    connect(overlaySource_, &WeatherOverlaySource::statusChanged, overlayStatusLabel_, &QLabel::setText);

    map_->setSites(model_->sites());
    if (model_->rowCount() > 0) {
        radarSelected(model_->index(0, 0));
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
    setWindowTitle(QStringLiteral("CappedWeather - NEXRAD"));
    setStyleSheet(QStringLiteral(
        "QMainWindow, QWidget { background: #0e1216; color: #d7dce0; }"
        "QComboBox, QToolButton { background: #171d22; border: 1px solid #36424a; padding: 5px; }"
        "QToolButton:checked { background: #174f69; border-color: #80d3ff; }"
        "QLabel#section { color: #8fa0aa; font-size: 11px; font-weight: 600; }"));

    auto* root = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(10);

    auto* controls = new QHBoxLayout();
    controls->setSpacing(8);

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

    statusLabel_ = new QLabel(QStringLiteral("Waiting for live NOAA data"), root);
    statusLabel_->setObjectName(QStringLiteral("section"));
    controls->addWidget(statusLabel_, 1);
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
    frameSlider_ = new QSlider(Qt::Horizontal, root);
    frameSlider_->setObjectName(QStringLiteral("frameSlider"));
    frameSlider_->setRange(0, 0);
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
    overlayStatusLabel_ = new QLabel(QStringLiteral("Waiting for warning overlays"), root);
    overlayStatusLabel_->setObjectName(QStringLiteral("section"));
    overlayStatusLabel_->setMinimumWidth(230);
    playback->addWidget(overlayStatusLabel_);
    rootLayout->addLayout(playback);

    setCentralWidget(root);
    updatePlaybackControls();
}

QString MainWindow::selectedRadarId() const {
    return selectedRadarId_;
}

void MainWindow::radarSelected(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    const auto radarId = index.data(RadarListModel::RadarIdRole).toString();
    if (radarId == selectedRadarId_ && !history_.empty()) return;
    selectedRadarId_ = radarId;
    stopAnimation();
    history_.clear();
    frameIndex_ = -1;
    updatePlaybackControls();
    map_->setSelectedRadar(radarId);
    map_->setLoading(true, QStringLiteral("Loading %1 radar data").arg(radarId));
    tiltCombo_->clear();
    tiltCombo_->addItem(QStringLiteral("Waiting for decoded tilts"));
    tiltCombo_->setEnabled(false);
    source_->requestRecent(radarId, 6);
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
        statusLabel_->setText(QStringLiteral("%1: %2").arg(radarId, status));
        if (status == QStringLiteral("no data") || status == QStringLiteral("no complete volume")) {
            map_->setLoading(false);
        }
    }
}

void MainWindow::sourceFailed(const QString& radarId, const QString& message) {
    if (radarId == QStringLiteral("CATALOG")) {
        statusLabel_->setText(QStringLiteral("Live catalog unavailable (%1); using embedded WSR-88D list")
                                  .arg(message));
        return;
    }
    model_->setStatus(radarId, QStringLiteral("error"));
    if (radarId == selectedRadarId()) {
        statusLabel_->setText(QStringLiteral("%1: %2").arg(radarId, message));
        map_->setLoading(false);
    }
}

void MainWindow::catalogReceived(const QByteArray& data) {
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        statusLabel_->setText(QStringLiteral("Official radar catalog unavailable; using real offline catalog"));
        return;
    }

    std::vector<RadarSite> sites;
    const auto features = document.object().value(QStringLiteral("features")).toArray();
    for (const auto& value : features) {
        const auto feature = value.toObject();
        const auto properties = feature.value(QStringLiteral("properties")).toObject();
        if (properties.value(QStringLiteral("stationType")).toString() != QStringLiteral("WSR-88D")) {
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
            QStringLiteral("WSR-88D").toStdString(),
            coordinates.at(1).toDouble(),
            coordinates.at(0).toDouble(),
            elevation.toDouble(),
        });
    }

    if (sites.empty()) {
        statusLabel_->setText(QStringLiteral("Official radar catalog empty; using real offline catalog"));
        return;
    }

    model_->replaceSites(std::move(sites));
    map_->setSites(model_->sites());
    statusLabel_->setText(QStringLiteral("Official NWS radar catalog loaded"));
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
    if (history_.empty()) {
        map_->setLoading(true, QStringLiteral("Downloading and decoding %1").arg(radarId));
    }
    statusLabel_->setText(QStringLiteral("%1: assembling %2 real volume chunks").arg(radarId).arg(chunkCount));
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
                             const QDateTime& receivedAt) {
    if (radarId != selectedRadarId() || volumeId != pendingVolumeId_) {
        return;
    }
    model_->setStatus(radarId, QStringLiteral("received"), receivedAt);
    const auto followLatest = history_.empty() || frameIndex_ == static_cast<int>(history_.size()) - 1;
    const auto selectedVolume = frameIndex_ >= 0 && frameIndex_ < static_cast<int>(history_.size())
                                    ? history_.at(static_cast<std::size_t>(frameIndex_)).volumeId
                                    : QString{};
    const auto duplicate = std::find_if(history_.begin(), history_.end(), [&volumeId](const RadarFrame& frame) {
        return frame.volumeId == volumeId;
    });
    if (duplicate == history_.end()) {
        history_.push_back({volumeId, std::move(pendingVolume_), receivedAt});
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
    statusLabel_->setText(QStringLiteral("%1: %2 real volume%3 in history")
                              .arg(radarId).arg(history_.size())
                              .arg(history_.size() == 1 ? QString{} : QStringLiteral("s")));
}

void MainWindow::rebuildTiltCombo() {
    struct TiltSummary {
        int number = 0;
        double angleTotal = 0.0;
        int rayCount = 0;
    };
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
    std::vector<TiltSummary> uniqueTilts;
    for (const auto& tilt : tilts) {
        const auto angle = std::round(tilt.angleTotal / std::max(tilt.rayCount, 1) * 10.0) / 10.0;
        auto duplicate = std::find_if(uniqueTilts.begin(), uniqueTilts.end(), [angle](const TiltSummary& other) {
            const auto otherAngle = std::round(other.angleTotal / std::max(other.rayCount, 1) * 10.0) / 10.0;
            return std::abs(otherAngle - angle) < 0.01;
        });
        if (duplicate == uniqueTilts.end()) {
            uniqueTilts.push_back(tilt);
        } else if (tilt.rayCount >= duplicate->rayCount) {
            *duplicate = tilt;
        }
    }
    tiltCombo_->clear();
    for (const auto& tilt : uniqueTilts) {
        const auto meanAngle = tilt.angleTotal / std::max(tilt.rayCount, 1);
        tiltCombo_->addItem(QStringLiteral("%1°").arg(meanAngle, 0, 'f', 1), tilt.number);
    }
    tiltCombo_->setEnabled(!uniqueTilts.empty());
    if (!uniqueTilts.empty()) {
        tiltCombo_->setCurrentIndex(0);
        map_->setElevationFilter(uniqueTilts.front().number);
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
    rebuildTiltCombo();
    frameLabel_->setText(QStringLiteral("%1 / %2  %3")
                             .arg(frameIndex_ + 1)
                             .arg(history_.size())
                             .arg(frame.dataTime.toLocalTime().toString(QStringLiteral("HH:mm:ss"))));
    updatePlaybackControls();
}

void MainWindow::advanceFrame() {
    if (history_.size() < 2) {
        stopAnimation();
        return;
    }
    showFrame((frameIndex_ + 1) % static_cast<int>(history_.size()));
}

void MainWindow::updatePlaybackControls() {
    const auto hasFrames = !history_.empty();
    const auto canAnimate = history_.size() > 1;
    frameSlider_->setRange(0, std::max(0, static_cast<int>(history_.size()) - 1));
    previousButton_->setEnabled(hasFrames && frameIndex_ > 0);
    nextButton_->setEnabled(hasFrames && frameIndex_ + 1 < static_cast<int>(history_.size()));
    playButton_->setEnabled(canAnimate);
    frameSlider_->setEnabled(canAnimate);
    speedCombo_->setEnabled(canAnimate);
    if (!hasFrames) frameLabel_->setText(QStringLiteral("No volume history"));
}

void MainWindow::stopAnimation() {
    animationTimer_->stop();
    if (playButton_->isChecked()) playButton_->setChecked(false);
    playButton_->setText(QStringLiteral("Play"));
}

} // namespace cappedweather
