#pragma once

#include "core/level2_decoder.hpp"
#include "app/weather_overlay_source.hpp"

#include <QMainWindow>

#include <vector>

class QByteArray;
class QDateTime;
class QModelIndex;
class QComboBox;
class QCheckBox;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;

namespace cappedweather {

class NexradSource;
class RadarMapCanvas;
class RadarListModel;
class WeatherOverlaySource;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void radarSelected(const QModelIndex& index);
    void requestSelectedRadar();
    void sourceStatusChanged(const QString& radarId, const QString& status);
    void sourceFailed(const QString& radarId, const QString& message);
    void catalogReceived(const QByteArray& data);
    void volumeStarted(const QString& radarId, const QString& volumeId, int chunkCount);
    void chunkDecompressed(const QString& radarId, const QString& volumeId, const QString& key,
                           const QByteArray& data, const QDateTime& receivedAt);
    void volumeReady(const QString& radarId, const QString& volumeId, int chunkCount,
                     const QDateTime& receivedAt);
    void showFrame(int index);
    void advanceFrame();

private:
    void buildUi();
    void rebuildTiltCombo();
    void updatePlaybackControls();
    void stopAnimation();
    QString selectedRadarId() const;

    struct RadarFrame {
        QString volumeId;
        DecodedChunk chunk;
        QDateTime dataTime;
    };

    RadarListModel* model_ = nullptr;
    RadarMapCanvas* map_ = nullptr;
    QComboBox* productCombo_ = nullptr;
    QComboBox* tiltCombo_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* overlayStatusLabel_ = nullptr;
    QSlider* frameSlider_ = nullptr;
    QToolButton* previousButton_ = nullptr;
    QToolButton* playButton_ = nullptr;
    QToolButton* nextButton_ = nullptr;
    QComboBox* speedCombo_ = nullptr;
    QCheckBox* warningsCheck_ = nullptr;
    QCheckBox* reportsCheck_ = nullptr;
    NexradSource* source_ = nullptr;
    WeatherOverlaySource* overlaySource_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    QTimer* overlayRefreshTimer_ = nullptr;
    QTimer* animationTimer_ = nullptr;
    QString selectedRadarId_;
    QString pendingVolumeId_;
    DecodedChunk pendingVolume_;
    std::vector<RadarFrame> history_;
    int frameIndex_ = -1;
};

} // namespace cappedweather
